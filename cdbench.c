#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include <mysql.h>

/*
 * If localhost (instead of 127.0.0.1) is defined, mysql automatically switches
 * to sockets
 */
#define HOSTNAME "127.0.0.1"
#define PORT	3306
#define USER	"ralf"
#define PASSWORD "abc12345"
#define DB "test"

#define F_RESULT "stats.csv"

#define SCHED SCHED_FIFO
#define SCHED_PRIO 42

#define RUNS 100000ULL

#define QUERY "--"
//#define QUERY "SELECT * FROM user;"

#define NSEC_PER_SEC		1000000000

#define ARRAY_SIZE(a) sizeof(a) / sizeof(a[0])

#define for_each_thread(t, i) \
	for (i = 0, t = thread; i < no_threads; i++, t++)

typedef unsigned long long latency_t;

struct thread_stat {
	pthread_t thread;
	pid_t tid;
	unsigned int thread_no;
	unsigned int cpu;

	bool threadstarted;

	long unsigned int runs;
	long unsigned int cycle;

	int policy;
	int priority;

	latency_t *latencies;
	latency_t act;
	latency_t min;
	latency_t avg;
	latency_t max;
};

static unsigned int shutdown;
static bool verbose = true;

/* Only required if swap is enabled */
static bool lockall = false;

static const float percentiles[] = {0.1, 0.5, 0.8, 0.9, 0.95, 0.99, 0.995};

static void __attribute__((noreturn)) usage(const char *prg)
{
	fprintf(stderr, "Usage: %s start_cpu cpus threads_per_cpu cpu_step\n", prg);
	exit(-1);
}

static void __attribute__((noreturn)) fatal(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("FATAL: ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static inline int calcdiff_ns(struct timespec t1, struct timespec t2)
{
	int diff;

	diff = NSEC_PER_SEC * (int64_t)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec);

	return diff;
}

static int query(MYSQL *m, const char *query, latency_t *latency)
{
	int err;
	unsigned int field_count;
	struct timespec now, then;

	MYSQL_RES *result = NULL;
	MYSQL_ROW row;

	/* CRITICAL SECTION START */
	clock_gettime(CLOCK_REALTIME, &then);

	err = mysql_query(m, query);
	if (err) {
		fprintf(stderr, "%s\n", mysql_error(m));
		return -1;
	}

	/*
	 * Documentation: "The mysql_field_count() function should be used to
	 * determine if there is a result set available."
	 */
	field_count = mysql_field_count(m);
	if (!field_count)
		goto crit_end;

	/* If results are available: Get them all. */
	result = mysql_store_result(m);
	if (!result) {
		fprintf(stderr, "store_result: %s\n", mysql_error(m));
		return -1;
	}

	//num_fields = mysql_num_fields(result);
	while ((row = mysql_fetch_row(result))) {
#ifdef DEBUG
		for(int i = 0; i < num_fields; i++)
			printf("%s ", row[i] ? row[i] : "NULL");
		printf("\n");
#endif
	}

crit_end:
	clock_gettime(CLOCK_REALTIME, &now);
	/* CRITICAL SECTION END */

	mysql_free_result(result);

	*latency = calcdiff_ns(now, then);
	return 0;
}

static void *benchthread(void *p)
{
	struct thread_stat *t = p;
	struct sched_param schedp;
	latency_t latency;
	cpu_set_t cpuset;
	MYSQL *m;
	int err;

	t->tid = gettid();

	CPU_ZERO(&cpuset);
	CPU_SET(t->cpu, &cpuset);

	err = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	if (err)
		fatal("Could not set affinity for thread %u on CPU %u\n", t->thread_no, t->cpu);

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = t->priority;
	err = sched_setscheduler(0, t->policy, &schedp);
	if (err)
		fatal("Could not set scheduler for thread %u on CPU %u\nError: %s\n", t->thread_no, t->cpu, strerror(errno));

	m = mysql_init(NULL);
	if (!m)
		fatal("%s\n", mysql_error(m));

	if (!mysql_real_connect(m, HOSTNAME, USER, PASSWORD, DB, PORT, NULL, 0)) {
		fatal("%s\n", mysql_error(m));
		goto close;
	}

	t->avg = t->max = 0;
	t->min = -1;
	t->threadstarted = true;

	for (t->cycle = 0; t->cycle < t->runs; t->cycle++) {
		err = query(m, QUERY, &latency);
		if (err)
			fatal("Query");

		t->latencies[t->cycle] = t->act = latency;
		t->avg += latency;

		if (latency < t->min)
			t->min = latency;
		if (latency > t->max)
			t->max = latency;
	}

close:
	mysql_close(m);

	/* back to normal */
	schedp.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &schedp);

	shutdown++;
	return NULL;
}

static void print_stat(struct thread_stat *t)
{
	if (!t->threadstarted) {
		printf("T:%2d (%5d) HALTED\n", t->thread_no, t->tid);
		return;
	}

	printf("T:%2d (%5d) CPU: %3u P:%2d C:%7lu Min: %7lld Act: %7lld Avg: %5lld Max: %8lld\n",
	       t->thread_no, t->tid, t->cpu, t->priority, t->cycle,
	       t->min, t->act, t->cycle ? t->avg / t->cycle : 0, t->max);
}

static int cmpfunc(const void *l_, const void *r_)
{
	latency_t *l = (latency_t *)l_, *r = (latency_t *)r_;

	if (*l < *r)
		return -1;
	if (*r < *l)
		return 1;

	return 0;
}

int main(int argc, const char **argv)
{
	unsigned int cpu_step, threads_per_cpu, start_cpu, no_threads, no_cpus, cpu, cpu_thread, i, j, run;
	struct thread_stat *thread;
	struct thread_stat *t;
	pthread_attr_t attr;
	float percentile;
	FILE *histogram;
	void *ret;
	int err;

	if (argc != 5)
		usage(argv[0]);

	start_cpu = atoi(argv[1]);
	no_cpus = atoi(argv[2]);
	threads_per_cpu = atoi(argv[3]);
	cpu_step = atoi(argv[4]);
	no_threads = no_cpus * threads_per_cpu;

	printf("Working on %u CPUs (%u threads per CPU, cpu step %u)\n", no_cpus, threads_per_cpu, cpu_step);

	thread = malloc(sizeof(*thread) * no_threads);
	if (!thread) {
		fprintf(stderr, "malloc()");
		return -1;
	}

	if (verbose)
		printf("MySQL client version: %s\n", mysql_get_client_info());

	/* lock all memory (prevent swapping) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			return -1;
		}
	
	for (cpu = 0, t = thread; cpu < no_cpus; cpu++) {
		for (cpu_thread = 0; cpu_thread < threads_per_cpu; cpu_thread++, t++) {
			t->thread_no = cpu * threads_per_cpu + cpu_thread;
			t->cpu = start_cpu + cpu * cpu_step;
			t->runs = RUNS;
			t->threadstarted = false;

			t->priority = SCHED_PRIO;
			t->policy = SCHED;

			t->latencies = malloc(sizeof(*t->latencies) * RUNS);
			if (!t->latencies)
				fatal("Allocating memory");
			/* Touch all pages. TBD: replace with mmap() */
			memset(t->latencies, 0, sizeof(*t->latencies) * RUNS);

			err = pthread_attr_init(&attr);
			if (err)
				fatal("error from pthread_attr_init for thread %d: %s\n", t->thread_no, strerror(err));

			err = pthread_create(&t->thread, &attr, benchthread, t);
			if (err)
				fatal("create thread: %u\n", t->thread_no);
		}
	}

	while (shutdown != no_threads) {
		if (verbose)
			for_each_thread(t, i)
				print_stat(t);
		usleep(100000);
		if (verbose)
			printf("\033[%dA", no_threads);
	}

	/* Join Threads */
	for_each_thread(t, i) {
		if (verbose)
			print_stat(t);
		err = pthread_join(t->thread, &ret);
		if (err)
			fatal("pthread_join: %d\n", err);
	}

	histogram = fopen(F_RESULT, "w");
	if (!histogram) {
		perror("fopen");
		err = -errno;
		goto cleanup;
	}

	for (i = 0; i < no_threads; i++) {
		fprintf(histogram, "%u (%u); ", thread[i].thread_no, thread[i].cpu);
	}
	fputc('\n', histogram);

	for (run = 0; run < RUNS; run++) {
		for (i = 0; i < no_threads; i++)
			fprintf(histogram, "%llu; ", thread[i].latencies[run]);
		fputc('\n', histogram);
	}

	/* Calculate some descriptive stats */
	for (i = 0; i < no_threads; i++) {
		qsort(thread[i].latencies, RUNS, sizeof(latency_t), cmpfunc);
		printf("Stats for Thread %u:\n", thread[i].thread_no);
		printf("\tMin: %7lld Avg: %5lld Max: %8lld\n", thread[i].min, thread[i].avg / thread[i].runs, thread[i].max);
		for (j = 0; j < ARRAY_SIZE(percentiles); j++) {
			percentile = percentiles[j];
			printf("\t%2.2f%%ile < %lluns\n", percentile * 100, thread[i].latencies[(long unsigned int)(RUNS * percentile)]);
		}
		printf("\n");
	}

	err = fclose(histogram);

cleanup:
	/* Clean up */
	for_each_thread(t, i)
		free(t->latencies);

	free(thread);

	mysql_library_end();

	return err;
}
