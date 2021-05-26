#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT	3306

#define CPUS 8
#define PRIORITY 42

#define MAXBUF   8192
#define LISTENQ  1024

#define __stringify_1(x)	#x
#define __stringify(x)		__stringify_1(x)

asm(
".macro inc_sample name, filename\n\t"
        ".pushsection .rodata\n\t"
        "\\name:\n\t"
                ".incbin \"\\filename\"\n\t"
        "\\name\\()_size:\n\t"
	".int \\name\\()_size - \\name\n\t"
        ".popsection\n\t"
".endm\n\t"
);

#define incbin(label, filename) \
	asm("inc_sample " __stringify(label) ", " filename "\n\t"); \
	extern const unsigned char label[]; \
	extern const unsigned int label##_size; \

incbin(response, "res/response.bin")
incbin(response_ok, "res/response_ok.bin")
incbin(server_hello, "res/server_hello.bin")
incbin(login_ok, "res/login_ok.bin")

struct thread_info {
	int fd;

	unsigned int cpu;
	unsigned int priority;
};

static void __attribute__((noreturn)) usage(const char *prg)
{
	fprintf(stderr, "%s start_cpu\n", prg);
	exit(-1);
}

static void *thread(void *p)
{
	struct thread_info *t = p;
	struct sched_param schedp;
	char buf[MAXBUF];
	cpu_set_t cpuset;
	ssize_t bread;
	int err;

	pthread_detach(pthread_self());

	CPU_ZERO(&cpuset);
	CPU_SET(t->cpu, &cpuset);

	err = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
	if (err) {
		fprintf(stderr, "Could not set affinity\n");
		return NULL;
	}

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = t->priority;
	err = sched_setscheduler(0, SCHED_FIFO, &schedp);
	if (err) {
		fprintf(stderr, "Could not set scheduler\n");
		return NULL;
	}

	printf("New server %u on CPU %u\n", gettid(), t->cpu);

	/* Step 1: Say Hello! */
	write(t->fd, server_hello, server_hello_size);

	/* Step 2: Receive Login Request */
	bread = read(t->fd, buf, sizeof(buf));

	/* Yep, you're in! */
	write(t->fd, login_ok, login_ok_size);

	while (1) {
		bread = read(t->fd, buf, sizeof(buf));
		if (bread <= 0)
			break;

		if (bread == 976 && *(uint32_t*)buf == 0x000003cc) {
			write(t->fd, response, response_size);
		}

		/* Yeah, just eat it… */
		write(t->fd, response_ok, response_ok_size);
	}

	printf("Shutting down %u\n", gettid());
	close(t->fd);
	free(t);

	return NULL;
}

int main(int argc, char **argv)
{
	socklen_t clientlen = sizeof(struct sockaddr_in);
	struct sockaddr_in clientaddr, serveraddr;
	unsigned int start_cpu, next_cpu;
	struct thread_info *t;
	pthread_t tid;
	int listenfd;
	int optval = 1;

	if (argc != 2)
		usage(argv[0]);

	start_cpu = atoi(argv[1]);

	if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return -1;
	}

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int)) < 0) {
		perror("setsocketopt");
		return -1;
	}

	bzero((char *) &serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons((unsigned short)PORT);
	if (bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0) {
		perror("bind");
		return -1;
	}

	if (listen(listenfd, LISTENQ) < 0) {
		perror("listen");
		return -1;
	}

	printf("Server started\nListening on 0.0.0.0:%u\n", PORT);
	printf("Will spawn server threads beginning from CPU %u\n", start_cpu);

	next_cpu = start_cpu;
	while (1) {
		t = malloc(sizeof(*t));
		if (!t) {
			fprintf(stderr, "malloc()");
			return -ENOMEM;
		}
		t->cpu = next_cpu;
		t->priority = PRIORITY;
		t->fd = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
		pthread_create(&tid, NULL, thread, (void*)t);

		if (++next_cpu > start_cpu + CPUS - 1)
			next_cpu = start_cpu;
	}

	return 0;
}
