// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
 
#include "global.h"

/*
 * Block I/O parameterization.  A basic block (BB) is the lowest size of
 * filesystem allocation, and must equal 512.  Length units given to bio
 * routines are in BB's.
 */

/* Assume that if we have BTOBB, then we have the rest */
#ifndef BTOBB
#define BBSHIFT         9
#define BBSIZE          (1<<BBSHIFT)
#define BBMASK          (BBSIZE-1)
#define BTOBB(bytes)    (((__u64)(bytes) + BBSIZE - 1) >> BBSHIFT)
#define BTOBBT(bytes)   ((__u64)(bytes) >> BBSHIFT)
#define BBTOB(bbs)      ((bbs) << BBSHIFT)
#define OFFTOBBT(bytes) ((__u64)(bytes) >> BBSHIFT)

#define SEEKLIMIT32     0x7fffffff
#define BBSEEKLIMIT32   BTOBBT(SEEKLIMIT32)
#define SEEKLIMIT       0x7fffffffffffffffLL
#define BBSEEKLIMIT     OFFTOBBT(SEEKLIMIT)
#endif

#ifndef OFFTOBB
#define OFFTOBB(bytes)  (((__u64)(bytes) + BBSIZE - 1) >> BBSHIFT)
#define BBTOOFF(bbs)    ((__u64)(bbs) << BBSHIFT)
#endif

#define	FSBTOBB(f)	(OFFTOBBT(FSBTOOFF(f)))
#define	BBTOFSB(b)	(OFFTOFSB(BBTOOFF(b)))
#define	OFFTOFSB(o)	((o) / blocksize)
#define	FSBTOOFF(f)	((f) * blocksize)

void usage(void)
{
    printf("usage: alloc [-b blocksize] [-d dir] [-f file] [-n] [-r] [-t]\n"
           "flags:\n"
           "    -n - non-interractive mode\n"
           "    -r - real time file\n"
           "    -t - truncate on open\n"
           "\n"  
           "commands:\n"  
           "    r   [offset] [length] - reserve\n"
           "    u   [offset] [length] - unreserve\n"
           "    a   [offset] [length] - alloc      *** identical to free\n"
           "    f   [offset] [length] - free       *** identical to alloc\n" 
           "    m/p [offset] [length] - print map\n"
           "    s                     - sync file\n"
           "    t   [offset]          - truncate\n"
           "    q                     - quit\n"
           "    h/?                   - this help\n");
           
}

int		fd = -1;
int		blocksize;
char		*filename;

/* params are in bytes */
void map(off64_t off, off64_t len)
{
    struct getbmap	bm[2];
    
    bzero(bm, sizeof(bm));

    bm[0].bmv_count = 2;
    bm[0].bmv_offset = OFFTOBB(off);
    if (len==(off64_t)-1) { /* unsigned... */
        bm[0].bmv_length = -1;
        printf("    MAP off=%lld, len=%lld [%lld-]\n", 
                (long long)off, (long long)len,
                (long long)BBTOFSB(bm[0].bmv_offset));
    } else {
        bm[0].bmv_length = OFFTOBB(len);
        printf("    MAP off=%lld, len=%lld [%lld,%lld]\n", 
                (long long)off, (long long)len,
                (long long)BBTOFSB(bm[0].bmv_offset),
                (long long)BBTOFSB(bm[0].bmv_length));
    }
    
    printf("        [ofs,count]: start..end\n");
    for (;;) {
#ifdef XFS_IOC_GETBMAP
	    if (xfsctl(filename, fd, XFS_IOC_GETBMAP, bm) < 0) {
#else
#ifdef F_GETBMAP
	    if (fcntl(fd, F_GETBMAP, bm) < 0) {
#else
bozo!
#endif
#endif
		    perror("getbmap");
		    break;
	    }

	    if (bm[0].bmv_entries == 0)
		    break;

	    printf("        [%lld,%lld]: ",
		    (long long)BBTOFSB(bm[1].bmv_offset),
		    (long long)BBTOFSB(bm[1].bmv_length));

	    if (bm[1].bmv_block == -1)
		    printf("hole");
	    else
		    printf("%lld..%lld",
			    (long long)BBTOFSB(bm[1].bmv_block),
			    (long long)BBTOFSB(bm[1].bmv_block +
				    bm[1].bmv_length - 1));
	    printf("\n");
    }
}

#ifdef HAVE_FALLOCATE
# define USE_LINUX_PREALLOCATE
enum linux_opno {
	FREESP = 0,
	ALLOCSP,
	UNRESVSP,
	RESVSP,
};

/* emulate the irix preallocation functions with linux vfs calls */
static int
linux_preallocate(
	int			fd,
	enum linux_opno		opno,
	const struct flock64	*f)
{
	struct stat		sbuf;
	int			ret;

	assert(f->l_whence == SEEK_SET);

	switch (opno) {
	case FREESP:
		return ftruncate(fd, f->l_start);
	case ALLOCSP:
		ret = fstat(fd, &sbuf);
		if (ret)
			return ret;

		return fallocate(fd, 0, sbuf.st_size,
				f->l_start - sbuf.st_size);
	case UNRESVSP:
		return fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				f->l_start, f->l_len);
	case RESVSP:
		return fallocate(fd, FALLOC_FL_KEEP_SIZE, f->l_start, f->l_len);
	}

	/* should never get here */
	errno = EINVAL;
	return -1;
}
#endif

int
main(int argc, char **argv)
{
	int		c;
	char		*dirname = NULL;
	int		done = 0;
	int		status = 0;
	struct flock64	f;
	off64_t		len;
	char		line[1024];
	off64_t		off;
	int		oflags;
	static char	*opnames[] = { "freesp",
				       "allocsp",
				       "unresvsp",
				       "resvsp" };
	int		opno;

#if defined(HAVE_FALLOCATE)
	/* see static function above */
#elif defined(XFS_IOC_FREESP64)
#define USE_XFSCTL
	/* Assume that if we have FREESP64 then we have the rest */
	static int	optab[] = { XFS_IOC_FREESP64,
				    XFS_IOC_ALLOCSP64,
				    XFS_IOC_UNRESVSP64,
				    XFS_IOC_RESVSP64 };
#elif defined(F_FREESP64)
#define USE_FCNTL
	static int	optab[] = { F_FREESP64,
				    F_ALLOCSP64,
				    F_UNRESVSP64,
				    F_RESVSP64 };
#else
# error Dont know how to preallocate space!
#endif
	int		rflag = 0;
	struct statvfs64	svfs;
	int		tflag = 0;
        int             nflag = 0;
	int		unlinkit = 0;
	int64_t		v;

	while ((c = getopt(argc, argv, "b:d:f:rtn")) != -1) {
		switch (c) {
		case 'b':
			blocksize = atoi(optarg);
			break;
		case 'd':
			if (filename) {
				printf("can't specify both -d and -f\n");
				exit(1);
			}
			dirname = optarg;
			break;
		case 'f':
			if (dirname) {
				printf("can't specify both -d and -f\n");
				exit(1);
			}
			filename = optarg;
			break;
		case 'r':
			rflag = 1;
			break;
		case 't':
			tflag = 1;
			break;
		case 'n':
                        nflag++;
                        break;
		default:
			printf("unknown option\n");
                        usage();
			exit(1);
		}
	}
	if (!dirname && !filename)
		dirname = ".";
	if (!filename) {
		static char	tmpfile[] = "allocXXXXXX";

		mkstemp(tmpfile);
		filename = malloc(strlen(tmpfile) + strlen(dirname) + 2);
		sprintf(filename, "%s/%s", dirname, tmpfile);
		unlinkit = 1;
	}
	oflags = O_RDWR | O_CREAT | (tflag ? O_TRUNC : 0);
	fd = open(filename, oflags, 0666);
        if (!nflag) {
            printf("alloc:\n");
	    printf("    filename %s\n", filename);
        }
	if (fd < 0) {
		perror(filename);
		exit(1);
	}
	if (!blocksize) {
		if (fstatvfs64(fd, &svfs) < 0) {
			perror(filename);
			status = 1;
			goto done;
		}
		blocksize = (int)svfs.f_bsize;
	}
	if (blocksize<0) {
		fprintf(stderr,"illegal blocksize %d\n", blocksize);
		status = 1;
		goto done;
	}
	printf("    blocksize %d\n", blocksize);
	if (rflag) {
		struct fsxattr a;

#ifdef XFS_IOC_FSGETXATTR
		if (xfsctl(filename, fd, XFS_IOC_FSGETXATTR, &a) < 0) {
			perror("XFS_IOC_FSGETXATTR");
			status = 1;
			goto done;
		}
#else
#ifdef F_FSGETXATTR
		if (fcntl(fd, F_FSGETXATTR, &a) < 0) {
			perror("F_FSGETXATTR");
			status = 1;
			goto done;
		}
#else
bozo!
#endif
#endif

		a.fsx_xflags |= XFS_XFLAG_REALTIME;

#ifdef XFS_IOC_FSSETXATTR
		if (xfsctl(filename, fd, XFS_IOC_FSSETXATTR, &a) < 0) {
			perror("XFS_IOC_FSSETXATTR");
			status = 1;
			goto done;
		}
#else
#ifdef F_FSSETXATTR
		if (fcntl(fd, F_FSSETXATTR, &a) < 0) {
			perror("F_FSSETXATTR");
			status = 1;
			goto done;
		}
#else
bozo!
#endif
#endif
	}
	while (!done) {
                char *p;
                
                if (!nflag) printf("alloc> ");
                fflush(stdout);
                if (!fgets(line, 1024, stdin)) break;
                
                p=line+strlen(line);
                if (p!=line&&p[-1]=='\n') p[-1]=0;
                
		opno = 0;
		switch (line[0]) {
		case 'r':
			opno++;
		case 'u':
			opno++;
		case 'a':
			opno++;
		case 'f':
			v = strtoll(&line[2], &p, 0);
			if (*p == 'b') {
				off = FSBTOOFF(v);
				p++;
			} else
				off = v;
			f.l_whence = SEEK_SET;
			f.l_start = off;
			if (*p == '\0')
				v = -1;
			else
				v = strtoll(p, &p, 0);
			if (*p == 'b') {
				len = FSBTOOFF(v);
				p++;
			} else
				len = v;
                        
                        printf("    CMD %s, off=%lld, len=%lld\n", 
                                opnames[opno], (long long)off, (long long)len);
                        
			f.l_len = len;
#if defined(USE_LINUX_PREALLOCATE)
			c = linux_preallocate(fd, opno, &f);
#elif defined(USE_XFSCTL)
			c = xfsctl(filename, fd, optab[opno], &f);
#elif defined(USE_FCNTL)
			c = fcntl(fd, optab[opno], &f);
#else
# error Dont know how to preallocate space!
#endif
			if (c < 0) {
				perror(opnames[opno]);
				break;
			}
                        
                        map(off,len);                        
			break;
		case 'p':
		case 'm':
			p = &line[1];
			v = strtoll(p, &p, 0);
			if (*p == 'b') {
				off = FSBTOOFF(v);
				p++;
			} else
				off = v;
			if (*p == '\0')
				len = -1;
			else {
				v = strtoll(p, &p, 0);
				if (*p == 'b')
					len = FSBTOOFF(v);
				else
					len = v;
			}
                        map(off,len);
			break;
		case 't':
			p = &line[1];
			v = strtoll(p, &p, 0);
			if (*p == 'b')
				off = FSBTOOFF(v);
			else
				off = v;
                        printf("    TRUNCATE off=%lld\n", (long long)off);
			if (ftruncate64(fd, off) < 0) {
				perror("ftruncate");
				break;
			}
			break;
                case 's':
                        printf("    SYNC\n");
                        fsync(fd);
                        break;
		case 'q':
                        printf("    QUIT\n");
			done = 1;
			break;
                case '?':
                case 'h':
                        usage();
                        break;
		default:
			printf("unknown command '%s'\n", line);
			break;
		}
	}
	if (!nflag) printf("\n");
done:
	if (fd != -1)
		close(fd);
	if (unlinkit)
		unlink(filename);
	exit(status);
	/* NOTREACHED */
}
