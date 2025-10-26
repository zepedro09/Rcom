// Application layer protocol implementation

#include "application_layer.h"
#include "link_layer.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void applicationLayer(const char *serialPort, const char *role, int baudRate,
                      int nTries, int timeout, const char *filename)
{
    LinkLayer link_layer;
    strcpy(link_layer.serialPort = serialPort);
    if(strcmp("tx", role) == 0){
        link_layer.role = LlTx;
    } else link_layer.role = LlRx;
    link_layer.baudRate = baudRate;
    link_layer.nRetransmissions = nTries;
    link_layer.timeout = timeout;

    if (llopen(link_layer) == -1) {
        return -1;
    }

    if(link_layer.role == LlTx){
        FILE *file = fopen(filename, "r");
        if(file == NULL) {
            printf("Can't find file \n")
            return -1;
        }

    }


    else if (link_layer.role == LlRx)
    {
    }
    else return -1;


    if (llclose(link_layer) == -1) {
        return -1;
    }
    

}
