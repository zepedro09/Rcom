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
        //2 control packets: first packet sends size of file and file name, the end one sends the same that the start one
        fseek(file, 0L, SEEK_END);
        int filesize = ftell(file);

        unsigned char *packet = (unsigned char*)malloc(MAX_PAYLOAD_SIZE);
        unsigned char types[2] = {0, 1};
        unsigned char *values[2];
        int lengths[2];

        values[0] = (unsigned char *) &filesize;
        lengths[0] = sizeof(filesize);
        values[1] = (unsigned char *) filename;
        lengths[1] = strlen(filename);

        int packetsize = createControlPacket(1, types, values, lengths, 2, packet);
        if (llwrite(packet, packetsize) == -1) {
            printf("Unable to send START\n");
            return -1;
        }
        free(packet);

        fseek(file, 0L, SEEK_SET);
        int bytesremaining = filesize;
        while (bytesremaining > 0)
        {
            printf("Sending data\n");
            int bytesread = bytesremaining > (MAX_PAYLOAD_SIZE-3) ? (MAX_PAYLOAD_SIZE-3) : bytesremaining;
            unsigned char* frame = (unsigned char*) malloc(bytesread);
            fread(frame, sizeof(char), bytesread, file);

            unsigned char *dataPacket = (unsigned char*)malloc(bytesread+3);
            datapacket[0] = 2;
            datapacket[1] = (bytesread) >> 8 & 0xFF;
            datapacket[2] = (bytesread) & 0xFF;
            memccpy(dataPacket+3, frame, bytesread);
            if(llwrite(dataPacket,bytesread+3)){
                printf("Unable to send DATA\n");
            }
            bytesremaining -=bytesread;
            free(frame);
            free(dataPacket);     
        }

        printf("Sending End\n");

        int endpacket = createControlPacket(1, types, values, lengths, 2, endpacket);
        if (llwrite(endpacket, packetsize) == -1) {
            printf("Unable to send START\n");
            return -1;
        }
        free(endpacket);
        fclose(file);
    }
    else if (link_layer.role == LlRx)
    {




    }
    else return -1;


    if (llclose(link_layer) == -1) {
        return -1;
    }
    

}
//pos: 1 - start, 3 - end;
int createControlPacket(int pos, const unsigned char types[], unsigned char *values[], int lengths[],
                         int nParams, unsigned char *packet)
{
    int packetLen = 0;

    packet[packetLen++] = (unsigned char) pos;
    for(int i = 0; i < nParams; i++){
        packet[packetLen++] = types[i];
        packet[packetLen++] = (unsigned char) lengths[i];
        memcpy(&packet[packetLen], values[i], lengths[i]);
    }

    return packetLen;
}


int readControlPacket();