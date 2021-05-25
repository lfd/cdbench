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

static const unsigned char server_hello[] = {
	0x59, 0x00, 0x00, 0x00, 0x0a, 0x35, 0x2e, 0x35,
	0x2e, 0x35, 0x2d, 0x31, 0x30, 0x2e, 0x35, 0x2e,
	0x31, 0x30, 0x2d, 0x4d, 0x61, 0x72, 0x69, 0x61,
	0x44, 0x42, 0x00, 0x05, 0x00, 0x00, 0x00, 0x2a,
	0x48, 0x6b, 0x62, 0x2a, 0x47, 0x69, 0x5c, 0x00,
	0xfe, 0xf7, 0xe0, 0x02, 0x00, 0xff, 0x81, 0x15,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00,
	0x00, 0x00, 0x6d, 0x34, 0x58, 0x3e, 0x6d, 0x2b,
	0x2a, 0x32, 0x5b, 0x57, 0x21, 0x3b, 0x00, 0x6d,
	0x79, 0x73, 0x71, 0x6c, 0x5f, 0x6e, 0x61, 0x74,
	0x69, 0x76, 0x65, 0x5f, 0x70, 0x61, 0x73, 0x73,
	0x77, 0x6f, 0x72, 0x64, 0x00
};

static const unsigned char login_ok[] = {
	0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
	0x40, 0x00, 0x00, 0x00, 0x07, 0x01, 0x05, 0x04,
	0x74, 0x65, 0x73, 0x74
};

static const unsigned char response_ok[] = {
	0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00
};

static void *thread(void *t)
{
	int fd = (int)(long unsigned int)t;
	char buf[MAXBUF];
	ssize_t bread;

	pthread_detach(pthread_self());

	/* Step 1: Say Hello! */
	write(fd, server_hello, sizeof(server_hello));

	/* Step 2: Receive Login Request */
	bread = read(fd, buf, sizeof(buf));
	printf("Login RQ: %lu\n", bread);

	/* Yep, you're in! */
	write(fd, login_ok, sizeof(login_ok));

	while (1) {
		bread = read(fd, buf, sizeof(buf));
		if (bread <= 0)
			break;

		/* Yeah, just eat it… */
		write(fd, response_ok, sizeof(response_ok));
	}

	close(fd);

	return NULL;
}

int main(void)
{
	socklen_t clientlen = sizeof(struct sockaddr_in);
	struct sockaddr_in clientaddr, serveraddr;
	int listenfd, connfdp;
	pthread_t tid;
	int optval=1;

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
		connfdp = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen);
		pthread_create(&tid, NULL, thread, (void*)(long unsigned int)connfdp);
	}

	return 0;
}


