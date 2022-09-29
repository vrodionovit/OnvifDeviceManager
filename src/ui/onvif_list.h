#ifndef ONVIF_LIST_H_ 
#define ONVIF_LIST_H_

#include "../soap/client.h"
#include <stdlib.h>

// Header
typedef struct {
  char * hostname;
} OnvifDevice;

typedef struct {
  int device_count;
  OnvifDevice *devices;
} OnvifDeviceList;

OnvifDeviceList* OnvifDeviceList__create(); 
void OnvifDeviceList__destroy(OnvifDeviceList* onvifDeviceList); 
void * OnvifDeviceList__insert_element(OnvifDeviceList* self, OnvifDevice record, int index);
void * OnvifDeviceList__remove_element(OnvifDeviceList* self, int index);
void * OnvifDeviceList__clear(OnvifDeviceList* self);

#endif