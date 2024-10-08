#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <semaphore.h>

#include <linux/videodev2.h>
#include <signal.h>

#include<sys/socket.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<netdb.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define HRES 640
#define VRES 480
#define PIXEL_SIZE 2
#define HRES_STR "640"
#define VRES_STR "480"

#define START_UP_FRAMES (12)
#define CAPTURE_FRAMES (180)
#define FRAMES_TO_ACQUIRE (CAPTURE_FRAMES + START_UP_FRAMES)

#define FRAMES_PER_SEC (1) 

#define DUMP_FRAMES

#define USEC_PER_MSEC (1000)
#define NANOSEC_PER_MSEC (1000000)
#define NANOSEC_PER_SEC (1000000000)
#define NUM_CPU_CORES (4)
#define TRUE (1)
#define FALSE (0)

#define RT_CORE (2)

#define NUM_THREADS (4)
#define MY_CLOCK_TYPE CLOCK_MONOTONIC_RAW


int abortTest=FALSE;
int abortS1=FALSE, abortS2=FALSE, abortS3=FALSE, abortS4 = FALSE;
sem_t semS1, semS2, semS3, semS4;
struct timespec start_time_val;
double start_realtime;
int dump=1;
int freq=0, acquire=0, store=0;
int my_freq1=0,my_freq2=0;

static timer_t timer_1;
static struct itimerspec itime = {{1,0}, {1,0}};
static struct itimerspec last_itime;
static unsigned long long seqCnt=0;

int my_size;

enum io_method {
	IO_METHOD_MMAP,
};

typedef struct
{
	int threadIdx;
} threadParams_t;


void Sequencer(int id);
void kill_process(void);

void *Service_1_acquisition(void *threadp);
void *Service_2_frame_process(void *threadp);
void *Service_3_frame_storage(void *threadp);
void *Service_4_user(void *threadp);

double getTimeMsec(void);
double realtime(struct timespec *tsptr);
void print_scheduler(void);

// Format is used by a number of functions, so made as a file global
static struct v4l2_format fmt;
struct v4l2_buffer buf; 

struct buffer 
{
	void   *start;
	size_t  length;
};

static char            *dev_name;
static enum io_method   io = IO_METHOD_MMAP;
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              out_buf;
static int              force_format=1;
static int              frame_count = (FRAMES_TO_ACQUIRE);


static double fnow=0.0, fstart=0.0, fstop=0.0;
static struct timespec time_now, time_start, time_stop;

static void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
	int r;

	do
	{
		r = ioctl(fh, request, arg);

	} while (-1 == r && EINTR == errno);

	return r;
}

//.pgm file header
char pgm_header[]="P5\n#9999999999 sec 9999999999 msec This is extra space for frame header pgm format sssssssssssssss \n"HRES_STR" "VRES_STR"\n255\n";
char pgm_dumpname[]="frames/test0000.pgm";
char uname[1000];
char *cmd = "uname -a";

//function to dump acquired frames in flash
static void dump_pgm(const void *p, int size, unsigned int tag, struct timespec *time)
{
	int written, i, total, dumpfd;

	snprintf(&pgm_dumpname[11], 9, "%04d", tag);
	strncat(&pgm_dumpname[15], ".pgm", 5);
	dumpfd = open(pgm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

	snprintf(&pgm_header[4], 11, "%010d", (int)time->tv_sec);
	strncat(&pgm_header[14], " sec ", 5);
	snprintf(&pgm_header[19], 11, "%010d", (int)((time->tv_nsec)/1000000));
	strncat(&pgm_header[29], " msec ", 6);
	snprintf(&pgm_header[35], 65, "%s", uname);
	strncat(&pgm_header[99], "\n"HRES_STR" "VRES_STR"\n255\n", 13);

	// subtract 1 from sizeof header because it includes the null terminator for the string
	written=write(dumpfd, pgm_header, sizeof(pgm_header)-1);

	total=0;

	do
	{
		written=write(dumpfd, p, size);
		total+=written;
	} while(total < size);

	close(dumpfd);

}

// always ignore first start-up frames
int framecnt=-(START_UP_FRAMES);
struct timespec frame_time;

unsigned char bigbuffer[(640*480*2)];
unsigned char ring_buffer[60][(640*480*2)];
int head=0, tail=0;
long int S1Cnt=-(START_UP_FRAMES);
int S2Cnt=-1;

/*
The store_frame function is responsible for storing a frame of data from a source buffer (bigbuffer) into a circular ring buffer (ring_buffer). The function is designed to manage a circular buffer with a fixed size of 60 frames, each represented by a two-dimensional array. The dimensions of the array correspond to the width (640) and height (480) of the frame. This function is responsible to decouple from I/O
*/
void store_frame() {

	for(int i=0; i<(640*480*2); i++) {
		ring_buffer[S2Cnt%60][i] = bigbuffer[i];
	}

	head++;
	head%=60;
}

/*
The process_frame function is designed to process a frame of data stored in memory, represented by a pointer p, and manipulate the pixel values to generate a modified frame stored in a destination buffer called bigbuffer. The function primarily focuses on processing the luminance (Y) component of the input frame, performing pixel value inversion to create a negative effect. The input size parameter indicates the total size of the frame data in bytes.
*/
static void process_frame(const void *p, int size)
{
	int i, newi, newsize=0;
	int y_temp, y2_temp, u_temp, v_temp;
	unsigned char *pptr = (unsigned char *)p;

	// Pixels are YU and YV alternating, so YUYV which is 4 bytes
	// We want Y, so YY which is 2 bytes
	//
	for(i=0, newi=0; i<size; i=i+4, newi=newi+2)
	{
		// Y1=first byte and Y2=third byte
   int y1 = pptr[i];
   int y2 = pptr[i + 2];
   bigbuffer[newi] = 255 - y1; // Invert y1 to make it negative
   bigbuffer[newi + 1] = 255 - y2; // Invert y2 to make it negative
	}

}

/*
The read_frame function is responsible for reading a video frame from a video capture device using the Video4Linux2 (V4L2) API, processing the frame if necessary, and managing the frame buffer queue. The function returns an indication of whether a frame was successfully read and processed.
*/
static int read_frame(void)
{
	unsigned int i;

	CLEAR(buf);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
	{
		switch (errno)
		{
		case EAGAIN:
			return 0;

		case EIO:
			/* Could ignore EIO, but drivers should only set for serious errors, although some set for
                           non-fatal errors too.
			 */
			return 0;


		default:
			printf("mmap failure\n");
			errno_exit("VIDIOC_DQBUF");
		}
	}

	assert(buf.index < n_buffers);

	framecnt++;

	if(framecnt == 0)
	{
		clock_gettime(CLOCK_MONOTONIC, &time_start);
		fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;
	}

	my_size=buf.bytesused;
	if(freq==1) {
		if(framecnt>=0)
			process_frame(buffers[buf.index].start, buf.bytesused);
	}

	if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
		errno_exit("VIDIOC_QBUF");

	return 1;
}


static void mainloop(void)
{
	struct timespec read_delay;
	struct timespec time_error;

	// Replace this with a delay designed for your rate
	// of frame acquitision and storage.
	//

	read_delay.tv_sec=0;
	read_delay.tv_nsec=33333;
	fd_set fds;
	struct timeval tv;
	int r;

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	/* Timeout. */
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	r = select(fd + 1, &fds, NULL, NULL, &tv);

	if (-1 == r)
	{
		if (EINTR == errno)
			//  continue;
		errno_exit("select");
	}

	if (0 == r)
	{
		fprintf(stderr, "select timeout\n");
		exit(EXIT_FAILURE);
	}

	if (read_frame())
	{
		if(nanosleep(&read_delay, &time_error) != 0)
			perror("nanosleep");
		else
		{

		}
	}

}

static void stop_capturing(void)
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
		errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
	unsigned int i;
	enum v4l2_buf_type type;

	for (i = 0; i < n_buffers; ++i)
	{
		printf("allocated buffer %d\n", i);
		struct v4l2_buffer buf;

		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");
	}
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
		errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(void)
{
	unsigned int i;

	for (i = 0; i < n_buffers; ++i)
		if (-1 == munmap(buffers[i].start, buffers[i].length))
			errno_exit("munmap");

	free(buffers);
}

static void init_mmap(void)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = 10;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
	{
		if (EINVAL == errno)
		{
			fprintf(stderr, "%s does not support "
					"memory mapping\n", dev_name);
			exit(EXIT_FAILURE);
		} else
		{
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	if (req.count < 2)
	{
		fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
		exit(EXIT_FAILURE);
	}

	buffers = calloc(req.count, sizeof(*buffers));

	if (!buffers)
	{
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory      = V4L2_MEMORY_MMAP;
		buf.index       = n_buffers;

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start =
				mmap(NULL /* start anywhere */,
						buf.length,
						PROT_READ | PROT_WRITE /* required */,
						MAP_SHARED /* recommended */,
						fd, buf.m.offset);

		if (MAP_FAILED == buffers[n_buffers].start)
			errno_exit("mmap");
	}
}

static void init_device(void)
{
	struct v4l2_capability cap;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	unsigned int min;

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
	{
		if (EINVAL == errno) {
			fprintf(stderr, "%s is no V4L2 device\n",
					dev_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
	{
		fprintf(stderr, "%s is no video capture device\n",
				dev_name);
		exit(EXIT_FAILURE);
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING))
	{
		fprintf(stderr, "%s does not support streaming i/o\n",
				dev_name);
		exit(EXIT_FAILURE);
	}

	/* Select video input, video standard and tune here. */


	CLEAR(cropcap);

	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
	{
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect; /* reset to default */

		if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
		{
			switch (errno)
			{
			case EINVAL:
				/* Cropping not supported. */
				break;
			default:
				/* Errors ignored. */
				break;
			}
		}

	}
	else
	{
		/* Errors ignored. */
	}


	CLEAR(fmt);

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (force_format)
	{
		printf("FORCING FORMAT\n");
		fmt.fmt.pix.width       = HRES;
		fmt.fmt.pix.height      = VRES;

		// Specify the Pixel Coding Formate here

		// This one works for Logitech C200
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

		fmt.fmt.pix.field       = V4L2_FIELD_NONE;

		if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
			errno_exit("VIDIOC_S_FMT");

		/* Note VIDIOC_S_FMT may change width and height. */
	}
	else
	{
		printf("ASSUMING FORMAT\n");
		/* Preserve original settings as set by v4l2-ctl for example */
		if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
			errno_exit("VIDIOC_G_FMT");
	}

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

	init_mmap();
}


static void close_device(void)
{
	if (-1 == close(fd))
		errno_exit("close");

	fd = -1;
}

static void open_device(void)
{
	struct stat st;

	if (-1 == stat(dev_name, &st)) {
		fprintf(stderr, "Cannot identify '%s': %d, %s\n",
				dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is no device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

	if (-1 == fd) {
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
				dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

static void usage(FILE *fp, int argc, char **argv)
{
	fprintf(fp,
			"Usage: %s [options]\n\n"
			"Version 1.3\n"
			"Options:\n"
			"-d | --device name   Video device name [%s]\n"
			"-h | --help          Print this message\n"
			"-m | --mmap          Use memory mapped buffers [default]\n"
			"-r | --read          Use read() calls\n"
			"-u | --userp         Use application allocated buffers\n"
			"-o | --output        Outputs stream to stdout\n"
			"-f | --format        Force format to 640x480 GREY\n"
			"-c | --count         Number of frames to grab [%i]\n"
			"",
			argv[0], dev_name, frame_count);
}

static const char short_options[] = "d:hmruofc:";

static const struct option
long_options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "help",   no_argument,       NULL, 'h' },
		{ "mmap",   no_argument,       NULL, 'm' },
		{ "read",   no_argument,       NULL, 'r' },
		{ "userp",  no_argument,       NULL, 'u' },
		{ "output", no_argument,       NULL, 'o' },
		{ "format", no_argument,       NULL, 'f' },
		{ "count",  required_argument, NULL, 'c' },
		{ 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{
	int user_input=0;
	int incorrect=0;

	if(argc > 1)
		dev_name = argv[1];
	else
		dev_name = "/dev/video0";

	FILE *fp;
	if((fp = popen(cmd,"r"))==NULL){
		printf("ERROR: Unable to run command [uname -r]\n");
	}
	while(fgets(uname,1028,fp)!=NULL){

	}
	pclose(fp);
	printf("HEADER: %s with size: %d\n",uname,strlen(uname));

	struct timespec current_time_val, current_time_res;
	double current_realtime, current_realtime_res;

	int i, rc, scope, flags=0;

	cpu_set_t threadcpu;
	cpu_set_t allcpuset;

	pthread_t threads[NUM_THREADS];
	threadParams_t threadParams[NUM_THREADS];
	pthread_attr_t rt_sched_attr[NUM_THREADS];
	int rt_max_prio, rt_min_prio, cpuidx;

	struct sched_param rt_param[NUM_THREADS];
	struct sched_param main_param;

	pthread_attr_t main_attr;
	pid_t mainpid;

	printf("Starting High Rate Sequencer Demo\n");
	clock_gettime(MY_CLOCK_TYPE, &start_time_val); start_realtime=realtime(&start_time_val);
	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
	clock_getres(MY_CLOCK_TYPE, &current_time_res); current_realtime_res=realtime(&current_time_res);
	printf("START High Rate Sequencer @ sec=%6.9lf with resolution %6.9lf\n", (current_realtime - start_realtime), current_realtime_res);

	printf("System has %d processors configured and %d available.\n", get_nprocs_conf(), get_nprocs());

	CPU_ZERO(&allcpuset);

	for(i=0; i < NUM_CPU_CORES; i++)
		CPU_SET(i, &allcpuset);

	printf("Using CPUS=%d from total available.\n", CPU_COUNT(&allcpuset));

	// initialize the sequencer semaphores
	//
	if (sem_init (&semS1, 0, 0)) { printf ("Failed to initialize S1 semaphore\n"); exit (-1); }
	if (sem_init (&semS2, 0, 0)) { printf ("Failed to initialize S2 semaphore\n"); exit (-1); }
	if (sem_init (&semS3, 0, 0)) { printf ("Failed to initialize S3 semaphore\n"); exit (-1); }
 if (sem_init (&semS4, 0, 0)) { printf ("Failed to initialize S5 semaphore\n"); exit (-1); }

	mainpid=getpid();

	rt_max_prio = sched_get_priority_max(SCHED_FIFO);
	rt_min_prio = sched_get_priority_min(SCHED_FIFO);

	rc=sched_getparam(mainpid, &main_param);
	main_param.sched_priority=rt_max_prio;
	rc=sched_setscheduler(getpid(), SCHED_FIFO, &main_param);
	if(rc < 0) perror("main_param");

	print_scheduler();

	pthread_attr_getscope(&main_attr, &scope);

	if(scope == PTHREAD_SCOPE_SYSTEM)
		printf("PTHREAD SCOPE SYSTEM\n");
	else if (scope == PTHREAD_SCOPE_PROCESS)
		printf("PTHREAD SCOPE PROCESS\n");
	else
		printf("PTHREAD SCOPE UNKNOWN\n");

	printf("rt_max_prio=%d\n", rt_max_prio);
	printf("rt_min_prio=%d\n", rt_min_prio);

	//set thread parameters
	for(i=1; i <= NUM_THREADS; i++)
	{
		rc=pthread_attr_init(&rt_sched_attr[i]);
		rc=pthread_attr_setinheritsched(&rt_sched_attr[i], PTHREAD_EXPLICIT_SCHED);
		rc=pthread_attr_setschedpolicy(&rt_sched_attr[i], SCHED_FIFO);

		rt_param[i].sched_priority=rt_max_prio-i;
		pthread_attr_setschedparam(&rt_sched_attr[i], &rt_param[i]);

		threadParams[i].threadIdx=i;
	}

	open_device();
	init_device();

	start_capturing();
		freq=1;
		my_freq1=1;
		my_freq2=1;
		acquire=100;
		store=100;

	// Servcie_1 = RT_MAX-1	@ 1 Hz
	//
	CPU_ZERO(&threadcpu);
	cpuidx=1;
	CPU_SET(cpuidx, &threadcpu);
	rc=pthread_attr_setaffinity_np(&rt_sched_attr[1], sizeof(cpu_set_t), &threadcpu);
	printf("Service thread 1 will run on %d CPU cores\n", cpuidx);
	rt_param[1].sched_priority=rt_max_prio-1;
	pthread_attr_setschedparam(&rt_sched_attr[1], &rt_param[1]);
	rc=pthread_create(&threads[1],               // pointer to thread descriptor
			&rt_sched_attr[1],         // use specific attributes
			//(void *)0,               // default attributes
			Service_1_acquisition,                 // thread function entry point
			(void *)&(threadParams[1]) // parameters to pass in
	);
	if(rc < 0)
		perror("pthread_create for service 1");
	else
		printf("pthread_create successful for service 1 at priority %d\n",rt_param[1].sched_priority);


	// Service_2 = RT_MAX-2	@ 1 Hz
	//
	CPU_ZERO(&threadcpu);
	cpuidx=(1);
	CPU_SET(cpuidx, &threadcpu);
	rc=pthread_attr_setaffinity_np(&rt_sched_attr[2], sizeof(cpu_set_t), &threadcpu);
	printf("Service thread 2 will run on %d CPU cores\n", cpuidx);
	rt_param[2].sched_priority=rt_max_prio-2;
	pthread_attr_setschedparam(&rt_sched_attr[2], &rt_param[2]);
	rc=pthread_create(&threads[2], &rt_sched_attr[2], Service_2_frame_process, (void *)&(threadParams[2]));
	if(rc < 0)
		perror("pthread_create for service 2");
	else
		printf("pthread_create successful for service 2 at priority %d\n", rt_param[2].sched_priority);


	// Service_3 = RT_MAX_PRIO-3	@ 1 Hz
	//
	CPU_ZERO(&threadcpu);
	cpuidx=(2);
	CPU_SET(cpuidx, &threadcpu);
	rc=pthread_attr_setaffinity_np(&rt_sched_attr[3], sizeof(cpu_set_t), &threadcpu);
	printf("Service thread 3 will run on %d CPU cores\n", cpuidx);
	rt_param[3].sched_priority=rt_max_prio-3;
	pthread_attr_setschedparam(&rt_sched_attr[3], &rt_param[3]);
	rc=pthread_create(&threads[3], &rt_sched_attr[3], Service_3_frame_storage, (void *)&(threadParams[3]));
	if(rc < 0)
		perror("pthread_create for service 3");
	else
		printf("pthread_create successful for service 3 at priority %d\n",rt_param[3].sched_priority);
	// Wait for service threads to initialize and await release by sequencer.
	
	//if(ethernet==1)
		//init_socket();
	printf("\nStarting sequencer\n");

	//printf("Press 1 to start frame dump\n");

	timer_create(CLOCK_REALTIME, NULL, &timer_1);

	//signal for sequencer function
	signal(SIGALRM, (void(*)()) Sequencer);
	

	itime.it_interval.tv_sec=0;
	itime.it_interval.tv_nsec=10000000;
	itime.it_value.tv_sec=0;
	itime.it_value.tv_nsec=10000000;

	timer_settime(timer_1, flags, &itime, &last_itime);

	for(i=1;i<NUM_THREADS;i++) {
		pthread_join(threads[i], NULL);
		printf("Thread %d joined\n", i);
	}

	clock_gettime(CLOCK_MONOTONIC, &time_stop);
	fstop = (double)time_stop.tv_sec + (double)time_stop.tv_nsec / 1000000000.0;

	// shutdown of frame acquisition service
	stop_capturing();

	printf("Total capture time=%lf, for %d frames, %lf FPS\n", (fstop-fstart), framecnt, (((double)framecnt) / (fstop-fstart)));

	uninit_device();
	close_device();
	fprintf(stderr, "\n");
	return 0;
}

//sequencer function to synchronize real time threads
void Sequencer(int id)
{
	struct timespec current_time_val;
	double current_realtime;
	int rc,flags=0;

	if(abortTest) {
		//disable interval timer

		itime.it_interval.tv_sec=0;
		itime.it_interval.tv_nsec=0;
		itime.it_value.tv_sec=0;
		itime.it_value.tv_nsec=0;

		timer_settime(timer_1, flags, &itime, &last_itime);
		printf("Disabling sequencer interval timer with abort=%d and %llu\n", abortTest, seqCnt);

		abortS1=TRUE, abortS2=TRUE;
	//	if(ethernet==1)
		//	abortS4=TRUE;
		sem_post(&semS1);
		sem_post(&semS2);

		return;
	}

	seqCnt++;

	// Servcie_1 = RT_MAX-1	@ 1 Hz
	if(((seqCnt % acquire) == 0)) sem_post(&semS1);

	// Service_2 = RT_MAX-2	@ 1 Hz
	if(((seqCnt % store) == 0)) sem_post(&semS2);

	if((seqCnt%2)==0)
		sem_post(&semS4);

}

//thread to get input from user to start dumping images in flash
void *Service_4_user(void *threadp) {
	int myInput = 1;
	int S5Cnt=-1;
	struct timespec current_time_val, exec_time_val;
	double current_realtime, exec1, exec2;
	threadParams_t *threadParams = (threadParams_t *)threadp;

	// Start up processing and resource initialization
	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
	syslog(LOG_CRIT, "S4, thread, sec=%6.9lf\n", current_realtime-start_realtime);
	while(!abortS4) {
		sem_wait(&semS4);

		exec1=getTimeMsec();
		S5Cnt++;

		dump=1;
		printf("Frame dump started\n");
		abortS4=TRUE;
		exec2=getTimeMsec();
		clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
		syslog(LOG_CRIT, "S4, 50 Hz on core %d for release, %ld, exec=%6.9lf\n", sched_getcpu(), S5Cnt, exec2-exec1);
		syslog(LOG_CRIT, "S4, 50 Hz on core %d for release, %ld, sec=%6.9lf\n", sched_getcpu(), S5Cnt, current_realtime-start_realtime);
	}
	pthread_exit((void *)0);
}

//thread to acquire frames captured by camera and process them
void *Service_1_acquisition(void *threadp)
{
	struct timespec current_time_val, exec_time_val;
	double current_realtime, exec1, exec2;
	threadParams_t *threadParams = (threadParams_t *)threadp;

	// Start up processing and resource initialization
	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
	syslog(LOG_CRIT, "S1 thread sec=%6.9lf\n", current_realtime-start_realtime);


	while(!abortS1) // check for synchronous abort request
	{
		// wait for service request from the sequencer, a signal handler or ISR in kernel
		sem_wait(&semS1);


		exec1=getTimeMsec();

		S1Cnt++;

		mainloop();
		exec2=getTimeMsec();

		// on order of up to milliseconds of latency to get time
		clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
		syslog(LOG_CRIT, "S1 %d Hz on core %d for release %ld, exec (ms)=%6.9lf\n",my_freq1, sched_getcpu(), S1Cnt, exec2-exec1);
		syslog(LOG_CRIT, "S1 %d Hz on core %d for release %ld, sec=%6.9lf\n",my_freq1, sched_getcpu(), S1Cnt, current_realtime-start_realtime);
	}
	pthread_exit((void *)0);
}

//thread to store acquired images in a buffer
void *Service_2_frame_process(void *threadp)
{
	struct timespec current_time_val, exec_time_val;
	double current_realtime, exec1, exec2;
	threadParams_t *threadParams = (threadParams_t *)threadp;

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
	syslog(LOG_CRIT, "S2 thread sec=%6.9lf\n", current_realtime-start_realtime);


	while(!abortS2)
	{
		sem_wait(&semS2);

		if((framecnt>=0) & (dump==1)) {

			exec1=getTimeMsec();
			S2Cnt++;

			store_frame();

			if(S2Cnt<=CAPTURE_FRAMES)
				sem_post(&semS3);

			exec2=getTimeMsec();
			clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
			syslog(LOG_CRIT, "S2 %d Hz on core (%d) for release %ld, exec(ms)=%6.9lf\n", my_freq2, sched_getcpu(), S2Cnt, exec2-exec1);
			syslog(LOG_CRIT, "S2 %d Hz on core (%d) for release %ld, sec=%6.9lf\n", my_freq2, sched_getcpu(), S2Cnt, current_realtime-start_realtime);
		}

		if(S2Cnt>=(CAPTURE_FRAMES)) {
			abortTest=TRUE;
		}
	}

	pthread_exit((void *)0);
}

int frame_dump=0;

//thread to dump images in flash
void *Service_3_frame_storage(void *threadp)
{
	struct timespec current_time_val, exec_time_val;
	double current_realtime, exec1, exec2;
	long int S3Cnt=-1;
	threadParams_t *threadParams = (threadParams_t *)threadp;

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
	syslog(LOG_CRIT, "S3 thread sec=%6.9lf\n", current_realtime-start_realtime);

	while(!abortS3)
	{
		sem_wait(&semS3);

		exec1=getTimeMsec();  
		S3Cnt++;

		clock_gettime(CLOCK_REALTIME, &frame_time);
		dump_pgm((ring_buffer+(S3Cnt%60)), (my_size/2), frame_dump, &frame_time);
		tail = tail+1;
		tail%=60;
		frame_dump++;

		exec2=getTimeMsec();

		clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime=realtime(&current_time_val);
		syslog(LOG_CRIT, "S3 %d Hz on core %d for release %ld exec=%6.9lf\n",my_freq2, sched_getcpu(), S3Cnt, exec2-exec1);
		syslog(LOG_CRIT, "S3 %d Hz on core %d for release %ld sec=%6.9lf\n",my_freq2, sched_getcpu(), S3Cnt, current_realtime-start_realtime);

		if(S3Cnt>=(CAPTURE_FRAMES))
			abortS3=TRUE;
	}


	pthread_exit((void *)0);
}

double getTimeMsec(void)
{
	struct timespec event_ts = {0, 0};

	clock_gettime(MY_CLOCK_TYPE, &event_ts);
	return ((event_ts.tv_sec)*1000.0) + ((event_ts.tv_nsec)/1000000.0);
}

double realtime(struct timespec *tsptr)
{
	return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec)/1000000000.0));
}

void print_scheduler(void)
{
	int schedType;

	schedType = sched_getscheduler(getpid());

	switch(schedType)
	{
	case SCHED_FIFO:
		printf("Pthread Policy is SCHED_FIFO\n");
		break;
	case SCHED_OTHER:
		printf("Pthread Policy is SCHED_OTHER\n"); exit(-1);
		break;
	case SCHED_RR:
		printf("Pthread Policy is SCHED_RR\n"); exit(-1);
		break;
	default:
		printf("Pthread Policy is UNKNOWN\n"); exit(-1);
	}
}