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
};

static void *thread(void *p)
{
	struct thread_info *t = p;
	char buf[MAXBUF];
	ssize_t bread;

	pthread_detach(pthread_self());
	printf("New server %u\n", gettid());

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

int main(void)
{
	socklen_t clientlen = sizeof(struct sockaddr_in);
	struct sockaddr_in clientaddr, serveraddr;
	struct thread_info *t;
	pthread_t tid;
	int listenfd;
	int optval = 1;

	if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;

	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int)) < 0)
		return -1;

	bzero((char *) &serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons((unsigned short)PORT);
	if (bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
		return -1;

	if (listen(listenfd, LISTENQ) < 0)
		return -1;

	while (1) {
		t = malloc(sizeof(*t));
		if (!t) {
			fprintf(stderr, "malloc()");
			return -ENOMEM;
		}
		t->fd = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
		pthread_create(&tid, NULL, thread, (void*)t);
	}

	return 0;
}
