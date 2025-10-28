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

        int endpacket = createControlPacket(3, types, values, lengths, 2, endpacket);
        if (llwrite(endpacket, packetsize) == -1) {
            printf("Unable to send START\n");
            return -1;
        }
        free(endpacket);
        fclose(file);
        llclose(link_layer);
    }
    else if (link_layer.role == LlRx)
    {
        FILE *file;
        unsigned char *packet = malloc(MAX_PAYLOAD_SIZE+100);
        int packetsize = llread(packet);
        if(packetsize == -1){
            printf("Error reading control packet\n");
        }else{
            int pos = packet[0];
            if(pos != 1){
                printf("Expected START control packet\n");
                return -1;
            }
            int filesize = 0;
            char filename[256];
            for(int i =1; i < packetsize; ){
                unsigned char type = packet[i++];
                unsigned char length = packet[i++];
                if(type == 0){ //size
                    for(int j =0; j < length; j++){
                        filesize = (filesize << 8) | packet[i + j];
                    }
                    i += length;
                } else if (type == 1){ //filename
                    memcpy(filename, &packet[i], length);
                    filename[length] = '\0';
                    file = fopen(filename, "w");
                    i += length;
                } else {
                    printf("Unknown parameter type\n");
                    return -1;
                }
            }
            free(packet);
            printf("Receiving file: %s of size %d bytes\n", filename, filesize);

            packetsize = 0;
            while (1) {
                while ((packetsize = llread(packet)) == -1);
                if (packet[0] == 2)
                {
                    int bytesread = packet[1] << 8 | packet[2];
                    fwrite(packet + 3, sizeof(char), bytesread, file)
                }
                else if(packet[0] == 3) //end
                {
                    int filesize_end = 0;
                    char filename_end[256];
                    for(int i =1; i < packetsize; ){
                        unsigned char type = packet[i++];
                        unsigned char length = packet[i++];
                        if(type == 0){ //size
                            for(int j =0; j < length; j++){
                                filesize_end = (filesize_end << 8) | packet[i + j];
                            }
                            i += length;
                        } else if (type == 1){ //filename   
                            memcpy(filename_end, &packet[i], length);
                            filename_end[length] = '\0';
                            i += length;
                        } else {
                            printf("Unknown parameter type\n");
                            return -1;
                        }
                    }
                    if (filesize_end != filesize || strcmp(filename_end, filename) != 0) {
                        printf("Mismatch in END control packet\n");
                        return -1;
                    }
                    printf("Correct END packet received\n");
                    free(packet);
                }
            }
            fclose(file);
            free(packet);
            free(filename);
            llclose(link_layer);
        }


    }
    else return -1;

    

}
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

