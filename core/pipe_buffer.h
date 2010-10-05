#ifndef _PIPE_BUFFER_H
#define _PIPE_BUFFER_H

#include <stdio.h>
#include <stdexcept>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <assert.h>
#include <list>

class PipeBuffer {
    public:
        PipeBuffer(int fd_, size_t block_size_) {
            fd = fd_;
            block_size = block_size_;
            current_read_buf = NULL;
            read_so_far = 0;
            eof = false;
        }

        ~PipeBuffer( ) { 
            fprintf(stderr, "%d frames left in buffer\n", ready_blocks.size( ));
        }


        /* poll all the pipes and update buffers when possible */
        static void update(PipeBuffer **list, int n) {
            /* legal to define inline? */
            struct pollfd *pfds = new struct pollfd[n];
            int i;

            /* poll for pipes with data ready */
            for (i = 0; i < n; ++i) {
                list[i]->fill_pollfd(&pfds[i]);
                pfds[i].events = POLLIN | POLLNVAL | POLLHUP;
            }
            if (poll(pfds, n, POLL_TIMEOUT) < 0) {
                delete [] pfds;
                throw std::runtime_error("poll failed");
            }

            /* for any pipe which is ready to go, read more data */
            for (i = 0; i < n; ++i) {
                if ((pfds[i].revents & POLLIN) && list[i]->want_poll( )) {
                    list[i]->read_more( );
                } else if (pfds[i].revents & POLLHUP) {
                    list[i]->set_eof( );
                }

                if (pfds[i].revents & POLLNVAL) {
                    fprintf(stderr, "buffer %d (fd %d) no longer valid\n", i, pfds[i].fd); 
                    list[i]->set_error( );
                }

            }

            delete [] pfds;

        }

        uint8_t *get_next_block( ) {
            uint8_t *ret;
            if (ready_blocks.empty( )) {
                return NULL;
            } else {
                ret = ready_blocks.front( );
                ready_blocks.pop_front( );
                return ret;
            }
        }

        void done_with_block(uint8_t *block_ptr) {
            free(block_ptr);
        }

        bool at_eof( ) {
            return eof;
        }

        bool stream_finished( ) {
            return (eof && ready_blocks.empty( ));
        }

    protected:
        int fd;
        size_t block_size;
        uint8_t *current_read_buf;
        size_t read_so_far;
        bool eof;
        std::list<uint8_t *> ready_blocks;

        void fill_pollfd(struct pollfd *pfd) {
            pfd->fd = fd;
        }

        bool want_poll( ) {
            return true;
        }

        void set_eof( ) {
            eof = true;
        }

        void set_error( ) {
            /* More sophistication someday. */
            eof = true;
        }

        void read_more( ) {
            ssize_t result;

            /* don't try reading from a broken pipe */
            if (eof) {
                return;
            }
            
            // allocate new block if needed
            if (current_read_buf == NULL) {
                current_read_buf = (uint8_t *)malloc(block_size);
                read_so_far = 0;
                if (current_read_buf == NULL) {
                    throw std::runtime_error("Failed to allocate new block");
                }
            }
        
            // read what we can from the pipe
            result = read(fd, current_read_buf + read_so_far, block_size - read_so_far);

            if (result == -1) {
                perror("read from pipe");
                throw std::runtime_error("failed to read from pipe");
            } else if (result == 0) {
                /* EOF - throw away any partial block for now */
                free(current_read_buf);
                current_read_buf = NULL;
                read_so_far = 0;
                eof = true;

                fprintf(stderr, "EOF\n");
            } else {
                read_so_far += result;
                
                /* we never ask for more than we can take so this is true */
                assert(read_so_far <= block_size);

                if (read_so_far == block_size) {
                    /* finished a block so put it on the queue */
                    ready_blocks.push_back(current_read_buf);
                    current_read_buf = NULL;
                    read_so_far = 0;
                }
            }


        }


        static const int POLL_TIMEOUT = 1; /* milliseconds */
};

#endif
