/*
 * Copyright (C) 2009 by Matthias Ringwald
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY MATTHIAS RINGWALD AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 *  hci_h4_transport.c
 *
 *  HCI Transport API implementation for basic H4 protocol
 *
 *  Created by Matthias Ringwald on 4/29/09.
 */

#include "../config.h"

#undef USE_NETGRAPH
#undef USE_HCI_READER_THREAD
// go back to sleep in 3s
#define HCI_WAKE_TIMER_MS 3000

#include <termios.h>  /* POSIX terminal control definitions */
#include <fcntl.h>    /* File control definitions */
#include <unistd.h>   /* UNIX standard function definitions */
#include <stdio.h>
#include <string.h>
#include <pthread.h> 

#include "debug.h"
#include "hci.h"
#include "hci_transport.h"
#include "hci_dump.h"

#if defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT)
/* iPhone power management support */
#define BT_WAKE_DEVICE  "/dev/btwake"
static int fd_wake = 0;
#endif

typedef enum {
    H4_W4_PACKET_TYPE,
    H4_W4_EVENT_HEADER,
    H4_W4_ACL_HEADER,
    H4_W4_PAYLOAD,
    H4_W4_PICKUP
} H4_STATE;

typedef struct hci_transport_h4 {
    hci_transport_t transport;
    data_source_t *ds;
    int uart_fd;    // different from ds->fd for HCI reader thread

#ifdef USE_HCI_READER_THREAD
    // synchronization facilities for dedicated reader thread
    int pipe_fds[2];
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
#if defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT)
    timer_source_t sleep_timer;
#endif
} hci_transport_h4_t;

// single instance
static hci_transport_h4_t * hci_transport_h4 = NULL;

static int  h4_process(struct data_source *ds);
static void dummy_handler(uint8_t packet_type, uint8_t *packet, uint16_t size); 
static      hci_uart_config_t *hci_uart_config;

#ifdef USE_HCI_READER_THREAD
static void *h4_reader(void *context);
static int  h4_reader_process(struct data_source *ds);
#endif
#if defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT)
static void h4_wake_on(void);
static void h4_wake_off(void);
static void h4_wake_timeout(struct timer *ts);
#endif

static  void (*packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = dummy_handler;

// packet reader state machine
static  H4_STATE h4_state;
static int bytes_to_read;
static int read_pos;

static uint8_t hci_packet[1+HCI_PACKET_BUFFER_SIZE]; // packet type + max(acl header + acl payload, event header + event data)

#if defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT)
static void h4_wake_on(void)
{
    if (!fd_wake) {
        fd_wake = open(BT_WAKE_DEVICE, O_RDWR);
        usleep(10000);
    }
    run_loop_remove_timer(&hci_transport_h4->sleep_timer);
    run_loop_set_timer(&hci_transport_h4->sleep_timer, HCI_WAKE_TIMER_MS);
    hci_transport_h4->sleep_timer.process = h4_wake_timeout;
    run_loop_add_timer(&hci_transport_h4->sleep_timer); 

    return;
}

static void h4_wake_off(void)
{
    run_loop_remove_timer(&hci_transport_h4->sleep_timer);
    if (fd_wake) {
        close(fd_wake);
        fd_wake = 0;
    }
    return;
}

static void h4_wake_timeout(struct timer *ts)
{
    h4_wake_off();
}

#endif  /* defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT) */

// prototypes
static int    h4_open(void *transport_config){
    hci_uart_config = (hci_uart_config_t*) transport_config;
    struct termios toptions;
    int flags = O_RDWR | O_NOCTTY;
#ifndef USE_HCI_READER_THREAD
    flags |= O_NONBLOCK;
#endif
    int fd = open(hci_uart_config->device_name, flags);
    if (fd == -1)  {
        perror("init_serialport: Unable to open port ");
        perror(hci_uart_config->device_name);
        return -1;
    }
    
    if (tcgetattr(fd, &toptions) < 0) {
        perror("init_serialport: Couldn't get term attributes");
        return -1;
    }
    speed_t brate = hci_uart_config->baudrate_init; // let you override switch below if needed
    switch(hci_uart_config->baudrate_init) {
        case 57600:  brate=B57600;  break;
        case 115200: brate=B115200; break;
#ifdef B230400
        case 230400: brate=B230400; break;
#endif
#ifdef B460800
        case 460800: brate=B460800; break;
#endif
#ifdef B921600
        case 921600: brate=B921600; break;
#endif
    }
    cfsetispeed(&toptions, brate);
    cfsetospeed(&toptions, brate);
    
    // 8N1
    toptions.c_cflag &= ~PARENB;
    toptions.c_cflag &= ~CSTOPB;
    toptions.c_cflag &= ~CSIZE;
    toptions.c_cflag |= CS8;

    if (hci_uart_config->flowcontrol) {
        // with flow control
        toptions.c_cflag |= CRTSCTS;
    } else {
        // no flow control
        toptions.c_cflag &= ~CRTSCTS;
    }
    
    toptions.c_cflag |= CREAD | CLOCAL;  // turn on READ & ignore ctrl lines
    toptions.c_iflag &= ~(IXON | IXOFF | IXANY); // turn off s/w flow ctrl
    
    toptions.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // make raw
    toptions.c_oflag &= ~OPOST; // make raw
    
    // see: http://unixwiz.net/techtips/termios-vmin-vtime.html
    toptions.c_cc[VMIN]  = 1;
    toptions.c_cc[VTIME] = 0;
    
    if( tcsetattr(fd, TCSANOW, &toptions) < 0) {
        perror("init_serialport: Couldn't set term attributes");
        return -1;
    }
    
    // set up data_source
    hci_transport_h4->ds = malloc(sizeof(data_source_t));
    if (!hci_transport_h4->ds) return -1;
    hci_transport_h4->uart_fd = fd;

#ifdef USE_HCI_READER_THREAD
    // init synchronization tools
    pthread_mutex_init(&hci_transport_h4->mutex, NULL);
    pthread_cond_init(&hci_transport_h4->cond, NULL);
    
	// create pipe
	pipe(hci_transport_h4->pipe_fds);
    
	// create reader thread
	pthread_t hci_reader_thread;
	pthread_create(&hci_reader_thread, NULL, &h4_reader, NULL);
    
    hci_transport_h4->ds->fd = hci_transport_h4->pipe_fds[0];
    hci_transport_h4->ds->process = h4_reader_process;
#else
    hci_transport_h4->ds->fd = fd;
    hci_transport_h4->ds->process = h4_process;
#endif
    run_loop_add_data_source(hci_transport_h4->ds);
    
    // init state machine
    bytes_to_read = 1;
    h4_state = H4_W4_PACKET_TYPE;
    read_pos = 0;

    return 0;
}

static int h4_close(void *transport_config){
    // first remove run loop handler
	run_loop_remove_data_source(hci_transport_h4->ds);
    
    // close device 
    close(hci_transport_h4->ds->fd);

#if defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT)
    // let module sleep
    h4_wake_off();
#endif

    // free struct
    free(hci_transport_h4->ds);
    hci_transport_h4->ds = NULL;
    return 0;
}

static int h4_send_packet(uint8_t packet_type, uint8_t * packet, int size){
    if (hci_transport_h4->ds == NULL) return -1;
    if (hci_transport_h4->uart_fd == 0) return -1;

#if defined (USE_BLUETOOL) && defined (USE_POWERMANAGEMENT)
    // wake Bluetooth module
    h4_wake_on();
#endif

    hci_dump_packet( (uint8_t) packet_type, 0, packet, size);
    char *data = (char*) packet;
    int bytes_written = write(hci_transport_h4->uart_fd, &packet_type, 1);
    while (bytes_written < 1) {
        usleep(5000);
        bytes_written = write(hci_transport_h4->uart_fd, &packet_type, 1);
    };
    while (size > 0) {
        int bytes_written = write(hci_transport_h4->uart_fd, data, size);
        if (bytes_written < 0) {
            usleep(5000);
            continue;
        }
        data += bytes_written;
        size -= bytes_written;
    }
    return 0;
}

static void   h4_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)){
    packet_handler = handler;
}

static void   h4_deliver_packet(void){
    if (read_pos < 3) return; // sanity check
    hci_dump_packet( hci_packet[0], 1, &hci_packet[1], read_pos-1);
    packet_handler(hci_packet[0], &hci_packet[1], read_pos-1);
    
    h4_state = H4_W4_PACKET_TYPE;
    read_pos = 0;
    bytes_to_read = 1;
}

static void h4_statemachine(void){
    switch (h4_state) {
            
        case H4_W4_PACKET_TYPE:
            if (hci_packet[0] == HCI_EVENT_PACKET){
                bytes_to_read = HCI_EVENT_HEADER_SIZE;
                h4_state = H4_W4_EVENT_HEADER;
            } else if (hci_packet[0] == HCI_ACL_DATA_PACKET){
                bytes_to_read = HCI_ACL_HEADER_SIZE;
                h4_state = H4_W4_ACL_HEADER;
            } else {
                log_error("h4_process: invalid packet type 0x%02x\n", hci_packet[0]);
                read_pos = 0;
                bytes_to_read = 1;
            }
            break;
            
        case H4_W4_EVENT_HEADER:
            bytes_to_read = hci_packet[2];
            h4_state = H4_W4_PAYLOAD;
            break;
            
        case H4_W4_ACL_HEADER:
            bytes_to_read = READ_BT_16( hci_packet, 3);
            h4_state = H4_W4_PAYLOAD;
            break;
            
        case H4_W4_PAYLOAD:
#ifdef USE_HCI_READER_THREAD
            h4_state = H4_W4_PICKUP;
#else
            h4_deliver_packet();
#endif
            break;
        default:
            break;
    }
}

static int    h4_process(struct data_source *ds) {
    if (hci_transport_h4->uart_fd == 0) return -1;

    int read_now = bytes_to_read;
    //    if (read_now > 100) {
    //        read_now = 100;
    //    }
    
    // read up to bytes_to_read data in
    ssize_t bytes_read = read(hci_transport_h4->uart_fd, &hci_packet[read_pos], read_now);
    // printf("h4_process: bytes read %u\n", bytes_read);
    if (bytes_read < 0) {
        return bytes_read;
    }
    
    // hexdump(&hci_packet[read_pos], bytes_read);
    
    bytes_to_read -= bytes_read;
    read_pos      += bytes_read;
    if (bytes_to_read > 0) {
        return 0;
    }
    
    h4_statemachine();
    return 0;
}

#ifdef USE_HCI_READER_THREAD
static int h4_reader_process(struct data_source *ds) {
    // get token
    char token;
    int tokens_read = read(hci_transport_h4->pipe_fds[0], &token, 1);
    if (tokens_read < 1) {
        return 0;
    }
    
    // hci_reader received complete packet, just pick it up
    h4_deliver_packet();
    
    // un-block reader
    pthread_mutex_lock(&hci_transport_h4->mutex);
    pthread_cond_signal(&hci_transport_h4->cond);
    pthread_mutex_unlock(&hci_transport_h4->mutex);
    return 0;
}

static void *h4_reader(void *context){
	while(1){
        // read up to bytes_to_read data in
        int bytes_read = read(hci_transport_h4->uart_fd, &hci_packet[read_pos], bytes_to_read);
        // error
        if (bytes_read < 0) {
            h4_state = H4_W4_PACKET_TYPE;
            read_pos = 0;
            bytes_to_read = 1;
            continue;
        }
        
        bytes_to_read -= bytes_read;
        read_pos      += bytes_read;

        if (bytes_to_read > 0) continue;
                    
        h4_statemachine();

        if (h4_state != H4_W4_PICKUP) continue;
        
		// notify main thread
        char data = 'h';
		write(hci_transport_h4->pipe_fds[1], &data, 1);
		
		// wait for response
		pthread_mutex_lock(&hci_transport_h4->mutex);
		pthread_cond_wait(&hci_transport_h4->cond,&hci_transport_h4->mutex);
		pthread_mutex_unlock(&hci_transport_h4->mutex);
	}
}
#endif

static const char * h4_get_transport_name(void){
    return "H4";
}

static void dummy_handler(uint8_t packet_type, uint8_t *packet, uint16_t size){
}

// get h4 singleton
hci_transport_t * hci_transport_h4_instance() {
    if (hci_transport_h4 == NULL) {
        hci_transport_h4 = malloc( sizeof(hci_transport_h4_t));
        hci_transport_h4->ds                                      = NULL;
        hci_transport_h4->transport.open                          = h4_open;
        hci_transport_h4->transport.close                         = h4_close;
        hci_transport_h4->transport.send_packet                   = h4_send_packet;
        hci_transport_h4->transport.register_packet_handler       = h4_register_packet_handler;
        hci_transport_h4->transport.get_transport_name            = h4_get_transport_name;
        hci_transport_h4->transport.set_baudrate                  = NULL;
        hci_transport_h4->transport.can_send_packet_now           = NULL;
    }
    return (hci_transport_t *) hci_transport_h4;
}
