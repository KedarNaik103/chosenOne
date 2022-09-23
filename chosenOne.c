
// gcc -Wall chosen_one.c -o chosen -lwiringPi

#define BCM2708_PERI_BASE 0x3F000000
#define GPIO_BASE (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */

#define PAGE_SIZE (4 * 1024)
#define BLOCK_SIZE (4 * 1024)

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

#include <wiringPi.h>
#include <wiringSerial.h>

#define PULSE_WIDTH 20 //Percentage value 

int mem_fd;
void *gpio_map;

// I/O access
volatile unsigned *gpio;

#define NS_PER_SEC 1000000000LL;

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio + ((g) / 10)) &= ~(7 << (((g) % 10) * 3))
#define OUT_GPIO(g) *(gpio + ((g) / 10)) |= (1 << (((g) % 10) * 3))
#define SET_GPIO_ALT(g, a) *(gpio + (((g) / 10))) |= (((a) <= 3 ? (a) + 4 : (a) == 4 ? 3  \
                                                                                     : 2) \
                                                      << (((g) % 10) * 3))

#define GPIO_SET *(gpio + 7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio + 10) // clears bits which are 1 ignores bits which are 0

#define GET_GPIO(g) (*(gpio + 13) & (1 << g)) // 0 if LOW, (1<<g) if HIGH

#define GPIO_PULL *(gpio + 37)     // Pull up/pull down
#define GPIO_PULLCLK0 *(gpio + 38) // Pull up/pull down clock

//
// Set up a memory regions to access GPIO
//
void setup_io()
{
    /* open /dev/mem */
    if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
    {
        printf("can't open /dev/mem \n");
        exit(-1);
    }

    /* mmap GPIO */
    gpio_map = mmap(
        NULL,                   // Any adddress in our space will do
        BLOCK_SIZE,             // Map length
        PROT_READ | PROT_WRITE, // Enable reading & writting to mapped memory
        MAP_SHARED,             // Shared with other processes
        mem_fd,                 // File to map
        GPIO_BASE               // Offset to GPIO peripheral
    );

    close(mem_fd); // No need to keep mem_fd open after mmap

    if (gpio_map == MAP_FAILED)
    {
        printf("mmap error %d\n", (int)gpio_map); // errno also set!
        exit(-1);
    }

    // Always use volatile pointer!
    gpio = (volatile unsigned *)gpio_map;

} // setup_io

int event_triggered = 0;
static void timer_callback(int sig, siginfo_t *si, void *uc)
{
    GPIO_SET = 1 << 9;
    event_triggered = 1;
}



int serial_port ;
char datastore[9];

int is_kpi_available(void)
{
    if (serialDataAvail(serial_port))
    {
        char flag = serialGetchar(serial_port); 
        if(flag == 0x21)
        {
            return 1;
        }
    }
    return 0;
}


int main(int argc, char *argv[])
{
    /* Process command line argumets */
    if(argv[1] == NULL)
    {
        printf("arg 1 missing: pulse period (msec), example sudo./chosen 100 m, sudo./chosen 100 s\n\n");
        return 1;
    }

    if (argv[2] == NULL)
    {
        printf("arg 2 missing: Master(m) or Slave(m), example sudo./chosen 100 m, sudo./chosen 100 s\n");
        printf("Assuming Slave Device\n\n");
    }

    /*Init UART for reading STM32*/
    if ((serial_port = serialOpen("/dev/ttyS0", 115200)) < 0) /* open serial port */
    {
        fprintf(stderr, "Unable to open serial device: %s\n", strerror(errno));
        return 1;
    }

    if (wiringPiSetup() == -1) /* initializes wiringPi setup */
    {
        fprintf(stdout, "Unable to start wiringPi: %s\n", strerror(errno));
        return 1;
    }

    int g;
    // Set up gpi pointer for direct register access
    setup_io();
    for (g = 7; g <= 11; g++)
    {
        INP_GPIO(g); // must use INP_GPIO before we can use OUT_GPIO
        OUT_GPIO(g);
    }

    /* Variables Definition */
    FILE *fp;

    struct itimerspec begin;

    struct timespec ts;

    timer_t timer_id;
    struct sigevent sev;
    struct sigaction sa;

    struct timespec pw, pw_remain;
    struct timespec ev;
    uint64_t timestamp;

    /* Pulse Period Definition */
    pw.tv_sec  = 0;
    pw.tv_nsec = (atoi(argv[1]) * 1000000 * PULSE_WIDTH) / 100;
    printf("Program will now attempt to schedule events with specified period\n\n");

    /*Establish Handler for timer*/
    printf("Establishing Handler for signal %d\n", SIGRTMIN);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = timer_callback;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGRTMIN, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    /* Create the Timer */
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &timer_id;
    if(timer_create(CLOCK_REALTIME, &sev, &timer_id) == -1)
    {
        perror("timer create");
        exit(EXIT_FAILURE);
    }

    /* Figuring the beginning of a second */
    clock_gettime(CLOCK_REALTIME, &ts);
    long part = (long)1000000000 - (long)ts.tv_nsec;
    begin.it_value.tv_sec = 2;
    begin.it_value.tv_nsec = (long)(part);
    begin.it_interval.tv_sec = 0;
    begin.it_interval.tv_nsec = atoi(argv[1]) * 1000000;
    
    if(timer_settime(timer_id, 0, &begin, NULL) == -1)
    {
        perror("timer_settime");
        exit(EXIT_FAILURE);
    }


    printf("%ld.%ld\n", (long)(long)ts.tv_sec, ts.tv_nsec);
    printf("%ld wrt %ld\n", part, ts.tv_nsec);
    
    while (1)
    {
        if(argv[2] == NULL) //A Slave Device
        {
            if( event_triggered == 1)
            {
                clock_gettime(CLOCK_REALTIME, &ev);
                clock_nanosleep(CLOCK_REALTIME, 0, &pw, &pw_remain);
                GPIO_CLR = 1 << 9;
                timestamp = ev.tv_sec * NS_PER_SEC;
                timestamp = timestamp + ev.tv_nsec;
                printf("Event scheduled at:\t%ld\n", timestamp);
                event_triggered = 0;
            }
        }
        else //A Master Device
        {
            if(event_triggered == 1)
            {
                /* Get Timestamp of event */
                clock_gettime(CLOCK_REALTIME, &ev);
                clock_nanosleep(CLOCK_REALTIME, 0, &pw, &pw_remain);
                GPIO_CLR = 1 << 9;
                /* Ask STM32 for offset */
                serialPutchar(serial_port, 0x21U);
                event_triggered = 0;
            }
            
            if(is_kpi_available() == 1)
            {
                int cnt;
                for(cnt  = 0; cnt < 10; cnt++)
                {
                    datastore[cnt] = serialGetchar(serial_port);
                }
                timestamp = ev.tv_sec * NS_PER_SEC;
                timestamp = timestamp + ev.tv_nsec;
                printf("measurement,method=CHOSENONE offset=%s \t%ld\n", datastore, timestamp);
                fp = fopen("/home/kedar/GPTP4L_KPI/chosen_one.txt", "a");
                fprintf(fp,"kpi,method=CHOSENONE offset=%s   %ld\n", datastore, timestamp);
                fclose(fp);
            }
        }
    }
    return 0;
}
