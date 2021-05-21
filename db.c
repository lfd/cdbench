#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

#define CPUS 4
#define START_CPU 8
#define THREADS_PER_CPU 2
#define THREADS (CPUS * THREADS_PER_CPU)

#define SCHED SCHED_FIFO
#define SCHED_PRIO 42

#define RUNS 100000

#define QUERY "--"
//#define QUERY "SELECT * FROM user;"

#define NSEC_PER_SEC		1000000000

#define for_each_thread(t, i) \
	for (i = 0, t = thread; i < THREADS; i++, t++)

typedef unsigned long long latency_t;

struct thread_stat {
	pthread_t thread;
	unsigned int cpu;
	unsigned int thread_no;
	unsigned int runs;

	int policy;
	int priority;

	latency_t *latencies;
	latency_t min;
	latency_t avg;
	latency_t max;
};

static int shutdown;

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
	latency_t latency;
	unsigned int i;
	MYSQL *m;
	int err;
	cpu_set_t cpuset;
	struct sched_param schedp;

	CPU_ZERO(&cpuset);
	CPU_SET(t->cpu, &cpuset);

	err = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	if (err)
		fatal("Could not set affinity for thread %u on CPU %u\n", t->thread_no, t->cpu);

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = t->priority;
	err = sched_setscheduler(0, t->policy, &schedp);
	if (err)
		fatal("Could not set scheduler for thread %u on CPU %u\n", t->thread_no, t->cpu);

	m = mysql_init(NULL);
	if (!m)
		fatal("%s\n", mysql_error(m));

	if (!mysql_real_connect(m, HOSTNAME, USER, PASSWORD, DB, PORT, NULL, 0)) {
		fatal("%s\n", mysql_error(m));
		goto close;
	}

	t->avg = t->max = 0;
	t->min = -1;

	for (i = 0; i < t->runs; i++) {
		err = query(m, QUERY, &latency);
		if (err)
			fatal("Query");

		t->latencies[i] = latency;
		t->avg += latency;

		if (latency < t->min)
			t->min = latency;
		if (latency > t->max)
			t->max = latency;
	}

	t->avg /= t->runs;

close:
	mysql_close(m);

	/* back to normal */
	schedp.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &schedp);

	shutdown++;
	return NULL;
}

int main(void)
{
	struct thread_stat thread[THREADS];
	struct thread_stat *t;
	pthread_attr_t attr;
	unsigned int cpu, cpu_thread, i;
	int err;
	void *ret;

	printf("MySQL client version: %s\n", mysql_get_client_info());

	// TBD: Memlockall
	
	for (cpu = 0, t = thread; cpu < CPUS; cpu++) {
		for (cpu_thread = 0; cpu_thread < THREADS_PER_CPU; cpu_thread++, t++) {
			t->thread_no = cpu * THREADS_PER_CPU + cpu_thread;
			t->cpu = START_CPU + cpu;
			t->runs = RUNS;

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

			// TBD: Set Attributes: Sched Policy + Affinity

			printf("Starting Thread %u on CPU %u\n", t->thread_no, t->cpu);
			err = pthread_create(&t->thread, &attr, benchthread, t);
			if (err)
				fatal("create thread: %u\n", t->thread_no);
		}
	}

	/* Make this prettier! */
	while (shutdown != THREADS)
		sleep(1);

	/* Cleanup */
	for_each_thread(t, i) {
		err = pthread_join(t->thread, &ret);
		if (err)
			fatal("pthread_join: %d\n", err);

		printf("Closed thread: %u Result: %lld Min: %llu Avg: %llu Max: %llu\n",
		       i, (long long int)ret, t->min, t->avg, t->max);

		free(t->latencies);
	}

	mysql_library_end();

	return err;
}
