#ifndef UHARDDOOM_BUFFER_H
#define UHARDDOOM_BUFFER_H

#include <linux/file.h>

long create_buffer_fd(struct file *filp, unsigned int size);

#endif  // UHARDDOOM_BUFFER_H
