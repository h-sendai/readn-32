#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "get_num.h"
#include "logUtil.h"
#include "my_signal.h"
#include "my_socket.h"
#include "readn.h"
#include "set_cpu.h"
#include "set_timer.h"
#include "rbcp.h"

int debug = 0;
volatile sig_atomic_t has_alarm = 0;
int ignore_data_mismatch = 0;
struct timeval tv_start;
int error_count = 0;
int ring_bell = 0;

int usage()
{
    char msg[] = "Usage: nread-32 [-b bufsize] [-I] remote_host:port\n"
                 "default port: 24\n"
                 "-d: debug\n"
                 "-h: help\n"
                 "-b bufsize: readn() buffer size. suffix k for kilo, m for m\n"
                 "-I: ignore error\n"
                 "-B: ring bell if error\n";

    fprintf(stderr, "%s\n", msg);

    return 0;
}

void sig_alarm(int signo)
{
    has_alarm = 1;
    return;
}

int print_rate(unsigned long interval_read_bytes, unsigned long interval_read_count,
    struct timeval tv_now, struct timeval tv_prev, int rcvbuf, unsigned int full_counter)
{
    struct timeval tv_interval, tv_elapsed;
    timersub(&tv_now, &tv_start, &tv_elapsed);
    timersub(&tv_now, &tv_prev, &tv_interval);

    double elapsed_sec  = tv_elapsed.tv_sec  + 0.000001*tv_elapsed.tv_usec;
    double interval_sec = tv_interval.tv_sec + 0.000001*tv_interval.tv_usec;
    // printf("interval_read_bytes: %ld\n", interval_read_bytes);
    double rx_rate_MB_s = (double) interval_read_bytes / interval_sec / 1024.0 / 1024.0;
    double rx_rate_Gb_s = (double) interval_read_bytes * 8 / interval_sec / 1000000000.0;
    printf("%.6f %.6f %.3f MB/s %.3f Gbps %ld %d %d %u\n", 
        elapsed_sec, interval_sec, rx_rate_MB_s, rx_rate_Gb_s, interval_read_count, rcvbuf, error_count, full_counter);
    fflush(stdout);
    return 0;
}

int write_to_disk(unsigned char *buf, int bufsize, char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (fp == NULL) {
        err(1, "fopen for %s", filename);
    }
    int n = fwrite(buf, 1 /* byte */, bufsize, fp);
    if (n != bufsize) {
        fprintf(stderr, "fwrite returuns with: %d\n", n);
        exit(1);
    }

    return 0;
}

int verify_data(unsigned char *buf, int bufsize)
{
    /* XXX: start number has been changed to 1 */
    static unsigned int seq_num = 1;
    unsigned int *int_p;

    // for (int i = 0; i < bufsize/sizeof(int); ++i) {
    //     int_p = (unsigned int *)&buf[i*sizeof(int)];
    //     unsigned int value_in_buf = *int_p;
    //     value_in_buf = ntohl(value_in_buf);
    // のようおに整数先頭バッファをじゅんじさしてde-referenceする手も
    // あるがこれは以下の方法より遅いようだ（測定してみた）

    // int_p = bufとしてfor (i = 0, ...) ループでint_p++する方法を
    // 使ってみた。
    // もし値が期待値でなかった場合はバッファ全体をファイルに保存し
    // あとから検証できるようにした。

    int n_num = bufsize / sizeof(int);
    int_p = (unsigned int *)buf;
    unsigned int value_in_buf;
    for (int i = 0; i < n_num; ++i) {
        //int_p = (unsigned int *)&buf[i*sizeof(int)];
        //unsigned int value_in_buf = *int_p;
        value_in_buf = ntohl(*int_p);
        if (debug) {
            fprintf(stderr, "seq_num: %u, value_in_buf %u\n", seq_num, value_in_buf);
        }
        if (value_in_buf != seq_num) {
            error_count ++;
            struct timeval now, elapsed;
            gettimeofday(&now, NULL);
            timersub(&now, &tv_start, &elapsed);
            //printf("%ld.%06ld\n", now.tv_sec, now.tv_usec);
            fprintf(stderr, "%ld.%06ld data mismatch.  expected: %u (0x %x), got %u (0x %x). diff: %u\n", elapsed.tv_sec, elapsed.tv_usec, seq_num, seq_num, value_in_buf, value_in_buf, value_in_buf - seq_num);
            if (ring_bell) {
                fprintf(stderr, "\a");
            }
            if (ignore_data_mismatch) {
                seq_num = value_in_buf;
            }
            else {
                char filename[64];
                pid_t pid = getpid();
                snprintf(filename, sizeof(filename), "invalid-data.%d", pid);
                write_to_disk(buf, bufsize, filename);
                return -1;
                // exit(1);
            }
            return -1;
        }
        int_p++;
        seq_num++;
    }
    
    return 0;
}

int child_proc(int pipe_command[2], int pipe_data[2], char *remote_host, int port)
{
    if (close(pipe_command[1]) < 0) {
        warnx("close() on pipe_command[1] (write end) (child)");
        killpg(0, SIGTERM);
    }
    if (close(pipe_data[0]) < 0) {
        warnx("close() on pipe_data[0] (read end) (child)");
        killpg(0, SIGTERM);
    }

    unsigned char counter[4];
    unsigned char command_buf[1];
    for ( ; ; ) {
        /* command
         * 'r' read the registers below
         * 'e' exit
         */
        __attribute__((unused)) int n = read(pipe_command[0], command_buf, sizeof(command_buf));
        if (command_buf[0] == 'r') {
            counter[0] = get_reg_byte("192.168.10.12", 0x10100000);
            counter[1] = get_reg_byte("192.168.10.12", 0x10110000);
            counter[2] = get_reg_byte("192.168.10.12", 0x10120000);
            counter[3] = get_reg_byte("192.168.10.12", 0x10130000);

            unsigned int value = 256*256*256*counter[3] +
                                     256*256*counter[2] +
                                         256*counter[1] +
                                             counter[0] ;
            int n = write(pipe_data[1], &value, sizeof(value));
            if (n < 0) {
                warnx("write() for data (child)");
                killpg(0, SIGTERM);
            }
        }
        else if (command_buf[0] == 'e') {
            exit(0);
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int port = 24; /* default port */
    struct timeval display_rate_interval = { 1, 0 }; /* 1 second */
    struct timeval tv_now, tv_prev;
    unsigned long interval_read_bytes = 0;
    unsigned long interval_read_count = 0;
    unsigned long total_read_bytes;
    int do_verify_data = 1;

    int c;
    int bufsize = 128*1024;
    while ( (c = getopt(argc, argv, "b:dhIBV")) != -1) {
        switch (c) {
            case 'b':
                bufsize = get_num(optarg);
                break;
            case 'B':
                ring_bell = 1;
                break;
            case 'd':
                debug = 1;
                break;
            case 'h':
                usage();
                exit(0);
                break;
            case 'I':
                ignore_data_mismatch = 1;
                break;
            case 'V':
                do_verify_data = 0;
                break;
            default:
                break;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc != 1) {
        usage();
        exit(1);
    }

    char *remote_host_info = argv[0];
    char *tmp = strdup(remote_host_info);
    char *remote_host = strsep(&tmp, ":");
    if (tmp != NULL) {
        port = strtol(tmp, NULL, 0);
    }

    int pipe_command[2];
    int pipe_data[2];

    if (pipe(pipe_command) < 0) {
        err(1, "pipe for command");
    }
    if (pipe(pipe_data) < 0) {
        err(1, "pipe for data");
    }

    pid_t pid = fork();
    if (pid == 0) {
        child_proc(pipe_command, pipe_data, remote_host, port);
        exit(0);
    }

    /* following is parent */
    if (close(pipe_command[0]) < 0) {
        err(1, "close for pipe_command[0] (read end) (parent)");
    }
    if (close(pipe_data[1]) < 0) {
        err(1, "close for pipe_data[1] (write end) (parent)");
    }
        

    int sockfd = tcp_socket();
    if (sockfd < 0) {
        exit(1);
    }

    if (connect_tcp(sockfd, remote_host, port) < 0) {
        exit(1);
    }

    my_signal(SIGALRM, sig_alarm);
    set_timer(display_rate_interval.tv_sec, display_rate_interval.tv_usec,
              display_rate_interval.tv_sec, display_rate_interval.tv_usec);

    gettimeofday(&tv_start, NULL);
    tv_now  = tv_start;
    tv_prev = tv_start;

    unsigned char *buf = malloc(bufsize);
    if (buf == NULL) {
        err(1, "malloc for buf (size: %d)\n", bufsize);
    }

    unsigned char command_read_reg = 'r';
    unsigned char __attribute__((unused)) command_exit     = 'e';

    /* XXX */
    /* after connect, the board send zero (0x00000000) twice. */
    /* We read 4 bytes for first zero (0x00000000) here and discard the data */
#if 0
    {
        unsigned char disard_data[4];
        int n = readn(sockfd, disard_data, sizeof(disard_data));
        if (n < 0) {
            err(1, "readn() for disard_data");
        }
        write(pipe_command[1], &command_read_reg, 1);
    }
#endif

    write(pipe_command[1], &command_read_reg, 1);
    unsigned int full_counter;
    for ( ; ; ) {
        if (has_alarm) {
            gettimeofday(&tv_now, NULL);
            int rcvbuf = get_so_rcvbuf(sockfd);
            // printf("so_rcvbuf: %d\n", rcvbuf);
            read(pipe_data[0], &full_counter, sizeof(full_counter));
            write(pipe_command[1], &command_read_reg, 1);
            print_rate(interval_read_bytes, interval_read_count, tv_now, tv_prev, rcvbuf, full_counter);
            has_alarm = 0;
            interval_read_bytes = 0;
            interval_read_count = 0;
            tv_prev = tv_now;
            fflush(stdout);
        }
        int n = readn(sockfd, buf, bufsize);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            else {
                err(1, "read error");
            }
        }
        if (n != bufsize) {
            fprintf(stderr, "read error.  readn(sockfd, buf, bufsize) is used in this program, but does not read bufsize (%d), but %d bytes\n", bufsize, n);
            exit(1);
        }
        interval_read_count += 1;
        interval_read_bytes += n;
        if (do_verify_data) {
            if (verify_data(buf, bufsize) < 0) {
                struct timeval tv_now, elapsed;
                gettimeofday(&tv_now, NULL);
                timersub(&tv_now, &tv_start, &elapsed);
                //write(pipe_command[1], &command_read_reg, 1);
                read(pipe_data[0], &full_counter, sizeof(full_counter));
                printf("%ld.%06ld full_counter: %u\n", elapsed.tv_sec, elapsed.tv_usec, full_counter);

                set_timer(0, 0, 0, 0);
                sleep(5);
                gettimeofday(&tv_now, NULL);
                timersub(&tv_now, &tv_start, &elapsed);
                write(pipe_command[1], &command_read_reg, 1);
                read(pipe_data[0], &full_counter, sizeof(full_counter));
                printf("%ld.%06ld full_counter: %u\n", elapsed.tv_sec, elapsed.tv_usec, full_counter);
                exit(0);
            }
        }
        total_read_bytes += n;
    }
        
    return 0;
}
