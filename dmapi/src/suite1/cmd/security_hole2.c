// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
	To read unallocated disk blocks in a filesystem, do
		cc -o test thisfile.c
		./test myfile
		cat myfile
*/

extern	char	*sys_errlist[];
extern  int     optind;
extern  char    *optarg;

char	*Progname;


static void
usage(void)
{
	int	i;

	fprintf(stderr, "usage:\t%s [-s size] pathname\n", Progname);
	fprintf(stderr, "\t-s size\t\tsize of file (default 10,000,000 bytes)\n");
	exit(1);
}


int
main(
	int	argc, 
	char	**argv)
{
	char	*pathname = NULL;
	off_t	size = 10000000;
	char	buff[1];
	int	method = F_RESVSP;
	flock_t	flock;
	int	opt;
	int	fd;

	Progname = strrchr(argv[0], '/');
	if (Progname) {
		Progname++;
	} else {
		Progname = argv[0];
	}

	/* Crack and validate the command line options. */

	while ((opt = getopt(argc, argv, "s:")) != EOF) {
		switch (opt) {
		case 's':
			size = atol(optarg);
			break;
		case '?':
			usage();
		}
	}
	if (optind + 1 != argc)
		usage();
	pathname = argv[optind];

	/* Create the file and write one byte at a large offset to create a
	   big hole in the middle of the file.
	*/

	if ((fd = open(pathname, O_RDWR|O_CREAT|O_TRUNC, 0666)) < 0) {
		perror("open failed");
		exit(1);
	}

	/* First allocate uninitialized blocks. */

	flock.l_whence = 0;
	flock.l_start = 0;
	flock.l_len = size;

	if (fcntl(fd, method, &flock) < 0) {
		perror("fcntl failed");
		exit(1);
	}

	/* Now seek out and write the byte. */

	if (lseek(fd, size, 0) < 0) {
		perror("lseek failed");
		exit(1);
	}
	buff[0] = '\0';
	if (write(fd, buff, 1) != 1) {
		perror("write failed");
		exit(1);
	}
	printf("%s created\n", pathname);
}
