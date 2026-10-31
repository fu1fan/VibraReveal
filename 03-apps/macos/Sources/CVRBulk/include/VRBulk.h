#ifndef VR_BULK_H
#define VR_BULK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct VRUSB VRUSB;
VRUSB* vr_usb_open(double timeout, char* error, size_t error_size);
int vr_usb_read(VRUSB*, void* data, size_t size);
int vr_usb_send(VRUSB*, const void* data, size_t size);
const char* vr_usb_error(VRUSB*);
void vr_usb_close(VRUSB*);
#ifdef __cplusplus
}
#endif
#endif
