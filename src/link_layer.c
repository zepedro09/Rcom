// Link layer protocol implementation

#include "link_layer.h"
#include "serial_port.h"
#include "utils.h"

#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>



#include <stdlib.h> // Para rand() e srand()
#include <time.h>   // Para time()

/*  
static int random_initialized = 0; 
#define REJ_PROBABILITY 5
*/

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source


volatile int alarmEnabled = FALSE;
volatile int alarmCount = 0;
static int sequenceNumber = 0;



int sendSupervisionFrame(LinkLayerRole role, unsigned char controlField);
int readSupervisionFrame(LinkLayerRole role, unsigned char c);
int sendIFrame(const unsigned char *data, int datasize, int seqNumber);
int readIFrame(LinkLayerRole role, unsigned char *dest, int *destsize, int seqNumber);
void alarmHandler(int signal);
void setupAlarm();
int replaceByte(unsigned char byte, unsigned char *res);
int destuffBytes(unsigned char *data, int dataSize, unsigned char *dest);
int stuffBytes(unsigned char *data, int dataSize, unsigned char *dest);
int createBCC2 (const unsigned char *data, int dataSize);



////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters)
{
    if (openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate) == -1) 
        return -1;

    if (connectionParameters.role == LlTx) {
        setupAlarm();
        while (alarmCount < connectionParameters.nRetransmissions) {
            if(sendSupervisionFrame(LlTx, C_SET) == -1){
                return -1;
            }
            printf("Sended set\n");
            alarm(3);
            alarmEnabled = TRUE;
            
            int UA = FALSE;

            while (alarmEnabled && !UA)
            {
                printf("Waiting for UA frame...\n");
                if(readSupervisionFrame(LlTx, C_UA) != -1){
                    printf("\nReceived UA frame <-\n");
                    UA = TRUE; 
                }
            }
            if(UA){
                alarm(0);
                alarmEnabled = FALSE;
                alarmCount = 0;
                return 0;
            }             
        }
        alarmCount = 0;
        return -1;
    } else if (connectionParameters.role == LlRx) {
        if (readSupervisionFrame(LlRx, C_SET) == -1) {
            closeSerialPort();
            return -1;
        }

        printf("Received SET frame\n");
        if (sendSupervisionFrame(LlRx, C_UA) == -1) {
            
            closeSerialPort();
            return -1;
        }
        
        printf("Connection established! \n");
        return 0;
    }

    return 0;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(const unsigned char *buf, int bufSize)
{
    while (alarmCount < 3) {
        if (sendIFrame(buf, bufSize, sequenceNumber) == -1) {
            return -1;
        }
        printf("I-Frame sent (Ns=%d)\n", sequenceNumber);

        alarmEnabled = TRUE;
        alarm(3);
        unsigned char RR = (sequenceNumber == 0) ? C_RR_1 : C_RR_0;
        unsigned char REJ = (sequenceNumber == 0) ? C_REJ_0 : C_REJ_1;
        while (alarmEnabled)
        {
            if(readSupervisionFrame(LlTx, RR) == 0) 
            {
                printf("Response received successfully!\n");
                alarm(0); 
                alarmEnabled = FALSE;
                alarmCount = 0;
                sequenceNumber = (sequenceNumber + 1) % 2;
                return 0; 
            }
            else if (readSupervisionFrame(LlTx, REJ) == 0)
            {
                printf("Received REJ frame\n");
                break;
            }
        }
    }
    return -1;
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////


int llread(unsigned char *packet)
{
    unsigned char RR = (sequenceNumber == 0) ? C_RR_1 : C_RR_0;
    unsigned char REJ = (sequenceNumber == 0) ? C_REJ_0 : C_REJ_1;
    int packetsizeval = 0;
    int *packetsize = &packetsizeval;
 
    int response = readIFrame(LlRx, packet, packetsize, sequenceNumber);
 
    if(response == 1){
        if (sendSupervisionFrame(LlRx, REJ) == -1) return -1;
        printf("Sent REJ \n");
        return -1;
    }else if(response == 0){
        if (sendSupervisionFrame(LlRx, RR) == -1) return -1;
        sequenceNumber = (sequenceNumber + 1) % 2;
        printf("Sent RR \n");
        return *packetsize;
    }else{
        printf("ERROR \n");
        return -1;
    }
}


/*
int llread(unsigned char *packet)
{
    // Inicializa o gerador aleatório na primeira chamada
    if (!random_initialized) {
        srand(time(NULL));
        random_initialized = 1;
    }
    
    unsigned char RR = (sequenceNumber == 0) ? C_RR_1 : C_RR_0;
    unsigned char REJ = (sequenceNumber == 0) ? C_REJ_0 : C_REJ_1;
    int packetsizeval = 0;
    int *packetsize = &packetsizeval;
    
    // Tenta ler o I-Frame
    int response = readIFrame(LlRx, packet, packetsize, sequenceNumber);
    
    // --- LÓGICA DE INJEÇÃO DE ERRO ---
    int inject_rej = 0;
    
    if (response == 0) { // O quadro foi lido corretamente (sem BCC2 ou Ns errado)
        // Se o número aleatório for 0, injeta REJ (1/REJ_PROBABILITY chance)
        if ((rand() % REJ_PROBABILITY) == 0) {
            inject_rej = 1;
        }
    }
    // ----------------------------------

    if(response == 1 || inject_rej){ // Se houve erro REAL (1) ou se injetamos erro (inject_rej)
        if (sendSupervisionFrame(LlRx, REJ) == -1) return -1;
        printf("Sent REJ (Injected: %s) \n", inject_rej ? "YES" : "NO");
        
        // Se o erro foi injetado, NÃO avançamos o sequenceNumber, forçando retransmissão
        if (inject_rej) {
            printf("[TEST] 😈 Injetado REJ para forçar retransmissão!\n");
            return -1;
        }
        
    } else if(response == 0){ // Sucesso REAL
        if (sendSupervisionFrame(LlRx, RR) == -1) return -1;
        sequenceNumber = (sequenceNumber + 1) % 2;
        printf("Sent RR \n");
        
    } else { // Outro erro (e.g., llread retorna -1)
        printf("ERROR \n");
        return -1;
    }
    
    return *packetsize;
}
*/


////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(LinkLayer connectionParameters)
{
    if (openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate) == -1) 
        return -1;

    if (connectionParameters.role == LlTx) {
        while (alarmCount < connectionParameters.nRetransmissions) {
            if(sendSupervisionFrame(LlTx, C_DISC) == -1){
                return -1;
            }
            printf("Sended DISC frame\n");

            alarm(3);
            alarmEnabled = TRUE;
            
            int DISC = FALSE;

            while (alarmEnabled && !DISC)
            {
                if(readSupervisionFrame(LlTx, C_DISC) != -1){
                    printf("\nReceived DISC frame <-\n");
                    DISC = TRUE;
                }
            }
            if(DISC){
                alarm(0);
                alarmEnabled = FALSE;
                alarmCount = 0;
                if(sendSupervisionFrame(LlTx, C_UA) == -1){
                    return -1;
                }
                printf("Sent UA frame\n");
                break;
            }  
            return -1;
        }
    } else if (connectionParameters.role == LlRx) {
        if (readSupervisionFrame(LlRx, C_DISC) == -1) {
            return -1;
        }
        printf("Received DISC frame\n");
        
        if (sendSupervisionFrame(LlRx, C_DISC) == -1) {
            return -1;
        }
        printf("Sent DISC frame\n");

    }

    printf("Connection closed! \nBye, Bye!! \n");
    return closeSerialPort();
}


int sendSupervisionFrame(LinkLayerRole role, unsigned char controlField)
{
    unsigned char sendA = (role == LlTx) ? A_T : A_R;
    unsigned char frame[5];
    frame[0] = FLAG;
    frame[1] = sendA;
    frame[2] = controlField;
    frame[3] = BCC1(sendA, controlField);
    frame[4] = FLAG;
    return writeBytesSerialPort(frame, sizeof(frame));
}
int readSupervisionFrame(LinkLayerRole role, unsigned char c)
{
    unsigned char A_role = (role == LlRx) ? A_T : A_R;
    int state = 0;
    unsigned char byte, bcc[2];
    
    while (role == LlTx ? alarmEnabled : TRUE) {
        readByteSerialPort(&byte); 
        switch (state) {
            case 0: 
                if (byte == FLAG) state = 1;
                break;
            case 1: 
                if (byte == A_role)
                {
                    bcc[0] = byte;
                    state = 2; 
                }
                else state = 0;
                break;
            case 2: 
                if(byte == c)
                {
                    bcc[1] = byte;
                    if(byte != c){
                        printf("Wrong c\n");
                        break;
                    } 
                    state = 3;
                }
                else state = 0;
                break;
            case 3: 
                if (byte == BCC1(bcc[0], bcc[1])) state = 4;
                else state = 0;
                break;
            case 4: 
                if (byte == FLAG) {
                    printf("Frame received (A=0x%02X, C=0x%02X)\n", bcc[0], bcc[1]);
                    return 0;
                } 
                else state = 0;
                break;
        }
    }
    alarmEnabled = FALSE;
    return -1; 
}






int sendIFrame(const unsigned char *data, int datasize, int seqNumber)
{

    unsigned char tmp[datasize + 1];
    memcpy(tmp, data, datasize);
    tmp[datasize] = (unsigned char) createBCC2(data, datasize);

    int max_stuffed = 2 * (datasize + 1) + 1;
    unsigned char *stuffed = malloc(max_stuffed);

    int stuffedsize = stuffBytes(tmp, datasize + 1, stuffed);
    int frame_size = stuffedsize + 5;
    int controlField;

    controlField = (seqNumber == 0) ? C_I_0 : C_I_1;

    unsigned char frame[frame_size];

    frame[0] = FLAG;
    frame[1] = A_T;
    frame[2] = controlField;
    frame[3] = BCC1(A_T, controlField);
    memcpy(&frame[4], stuffed, stuffedsize);
    frame[frame_size - 1] = FLAG;
    free(stuffed);

    return writeBytesSerialPort(frame, sizeof(frame));
}


int readIFrame(LinkLayerRole role, unsigned char *dest, int *destsize, int seqNumber)
{
    unsigned char expectedA = A_T;
    unsigned char expectedC = (seqNumber == 0) ? C_I_0 : C_I_1;
    unsigned char bcc1[2];
    int state = 0;
    int tmpSize = MAX_PAYLOAD_SIZE * 2 + 8;
    unsigned char *tmp = malloc(tmpSize);
    int tmpLen = 0;
    
    printf(sequenceNumber == 0 ? "Expecting Ns=0\n" : "Expecting Ns=1\n");
    while (TRUE)
    {
        unsigned char byte;
        int res = readByteSerialPort(&byte);
        if (res <= 0) continue;
        switch (state)
        {
        case 0: // Flag
            if (byte == FLAG) state = 1;
            break;
        case 1: // A
            if (byte == expectedA) {
                bcc1[0] = byte;
                state = 2;
            } 
            else if (byte != FLAG) state = 0;
            break;
        case 2: // C
            if (byte == expectedC) {
                bcc1[1] = byte;
                state = 3;
            }
            else if (byte == FLAG) state = 1;
            else state = 0;
            break;
        case 3: // BCC1
            if (byte == BCC1(bcc1[0], bcc1[1])) state = 4;
            else if (byte == FLAG) state = 1;
            else state = 0;
            break;
        case 4: //Flag, D and BCC2
            if (byte == FLAG) {
                int destlen = destuffBytes(tmp, tmpLen, dest);
                free(tmp);
                if(destlen < 1) return -1;
                int datalen = destlen - 1;
                int bcc2 = createBCC2(dest, datalen);
                if(bcc2 != dest[datalen]){
                    printf("BCC2 error\n");
                    return 1; //error send REJ
                }
                *destsize = datalen;
                return 0; //success send RR
            }
            else{
                tmp[tmpLen++] = byte;
            }
            break;
        }
    }

    printf("Timeout - send REJ\n");
    return 1; 
}


int createBCC2 (const unsigned char *data, int dataSize){
    int bcc2 = 0x00;

    for(int i =0; i < dataSize; i++){
        bcc2 ^= data[i];
    }

    return bcc2;
}

int stuffBytes(unsigned char *data, int dataSize, unsigned char *dest){
    int max_size = 2 * dataSize + 8;

    int dest_size = 0;
    for(int i =0; i < dataSize; i++){
        unsigned char temp = data[i];
        int size_added = replaceByte(temp, &dest[dest_size]);

        if(size_added == -1) return -1;
        if(dest_size + size_added > max_size) return -1;

        dest_size += size_added;
        
    }
    return dest_size;
}

int destuffBytes(unsigned char *data, int dataSize, unsigned char *dest){

    if (!data || !dest || dataSize < 0) return -1;

    int dest_size = 0;

    for(int i =0; i < dataSize; i++){
        if(data[i] == ESC){
            if (i + 1 >= dataSize) return -1;
            if(data[i + 1] == ESC_FLAG) dest[dest_size++] = FLAG;
            else if(data[i + 1] == ESC_ESC) dest[dest_size++] = ESC;
            else return -1; 
            i++;
        }
        else dest[dest_size++] = data[i];
    }
    return dest_size;

}


int replaceByte(unsigned char byte, unsigned char *res){
    if(!res) return -1;

    if(byte == FLAG){
        res[0] = ESC;
        res[1] = ESC_FLAG;
        return 2;
    }
    else if(byte == ESC){
        res[0] = ESC;
        res[1] = ESC_ESC;
        return 2;
    }
    else{
        res[0] = byte;
        return 1;
    }
    
    
}

void alarmHandler(int signal)
{
    alarmEnabled = FALSE;
    alarmCount++;

    printf("Alarm #%d received\n", alarmCount);
}


void setupAlarm() {
    struct sigaction act = {0};
    act.sa_handler = &alarmHandler;
    if (sigaction(SIGALRM, &act, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}



