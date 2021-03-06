#include "OpenNI.h"
#include "utils.h"
#include "minilzo.h"
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <fstream>
#include <bitset>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

using namespace openni;

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//-------------------GLOBAL VARIABLES AND STRUCTURES------------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//NETWORKING VARIABLES
//Server address and port to use for the socket
//#define SERVER_IP "192.168.1.134" //Server desktop pis
#define SERVER_IP "192.168.1.136" //Server portatil pis
//#define SERVER_IP "147.83.179.2" //Server Armengod
#define PORT 8080

//Tamany del buffer a utilitzar per al socket
#define SOCK_BUFF_SIZE 1000

//DATA POINTERS TO CAMERA DATA
uint16_t* depthData;
uint8_t* imageData;


//FILTERING VARIABLES
//Tamany de la finestra per a filtratge (e.g. 3x3 = 9, 5x5 = 25, 7x7 = 49, etc)
#define WINDOW_SIZE 49
#define WINDOW_SIDE (int)sqrt((double)WINDOW_SIZE)
#define WINDOW_SIDE_HALF WINDOW_SIDE/2 //Util per a offsets
#define WALL_OFFSET (uint16_t) 5000 //Offset inicial de les "parets" de la matriu secundaria


//COMPRESSION STRUCTURES AND VARIABLES
//Flag per determinar si utilitzem compressio o no
#define COMPRESSION 1

//Array temporal de sortida de lzo, ~1MB
static unsigned char __LZO_MMODEL lzoArray [1000000];

//Memòria de treball per a la compressió
#define HEAP_ALLOC(var,size) \
	lzo_align_t __LZO_MMODEL var [ ((size) + (sizeof(lzo_align_t) - 1)) / sizeof(lzo_align_t) ]

static HEAP_ALLOC(wrkmem, LZO1X_1_MEM_COMPRESS);


//MULTITHREAD STRUCTURES AND VARIABLES//
//Estructura de dades a utilitzar com a parametre per als threads
struct threadArgs {
	int threadID;
	int firstRow;
	int lastRow;
	int matrixWidth;
	uint16_t *outputData;
};

//Flags a utilitzar per a sincronitzar threads
volatile int thread1Start = 0;
volatile int thread2Start = 0;
volatile int thread3Start = 0;
		
//Declaració de threads i estructures
threadArgs threadArgs1, threadArgs2, threadArgs3;
pthread_t thread1, thread2, thread3;

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//-------------------AUXILIARY AND HELPER FUNCTIONS-------------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//Function to sort a given window (array). Used for median filtering.
void insertSort(uint16_t window[]){
	uint16_t aux;
	int i, j;
	for(i = 0; i < WINDOW_SIZE; ++i){
		aux = window[i];
		for(j = i -1; j >= 0 && aux < window[j]; --j){
			window[j+1] = window[j];
		}
		window[j+1] = aux;
	}
}


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//-----------------------WORKER THREADS FUNCTION----------------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//Function to do a median windowed filtering of a subset of data (matrix).
//threadData contains the width of the matrix and the initial and end row to process.
void *medianFilterWorker(void *threadData){
	//Global variables to use, defined at main
	uint16_t *filteredDepthData = ((threadArgs*)threadData)->outputData;

	int firstRow = ((threadArgs*)threadData)->firstRow;
	int lastRow = ((threadArgs*)threadData)->lastRow;
	int matrixWidth = ((threadArgs*)threadData)->matrixWidth;

	uint16_t median;
	uint16_t window[WINDOW_SIZE];

	//Depenent del thread on som, mirem un flag o un altre
	volatile int *threadStart;
	switch(((threadArgs*)threadData)->threadID){
		case 1:
			threadStart = &thread1Start;
			break;
		case 2:
			threadStart = &thread2Start;
			break;
		case 3:
			threadStart = &thread3Start;
			break;
		//No hauria de passar
		default:
			threadStart = &thread1Start;
	}

	//DEBUG
	std::cout << "Thread " << ((threadArgs*)threadData)->threadID << " created, flag memory direction: " << threadStart  << "\n\n";

	//El thread sempre esta a la espera de que es notifiqui que pot començar
	while(1){
		while(!*threadStart);
		//DEBUG
		//std::cout << "Thread " << ((threadArgs*)threadData)->threadID << " processing, starting at [" << firstRow << "," << WINDOW_SIDE_HALF << "]\n\n";
		for(int i = firstRow; i <= lastRow; ++i){
		       for(int j = WINDOW_SIDE_HALF; j < matrixWidth - WINDOW_SIDE_HALF; ++j){
			       //Update dels elements de la finestra
			       for(int k = 0; k < WINDOW_SIZE; ++k){
						//std::cout << "Thread " << ((threadArgs*)threadData)->threadID << " processing [" << i << "," << j << "], window[" << k << "]\n";
						   window[k] = *(depthData + (matrixWidth * (i + ((k / WINDOW_SIDE) - WINDOW_SIDE_HALF)) + (j + (k % WINDOW_SIDE) - WINDOW_SIDE_HALF)));
			       } 
			 	//Sort dels valors de la finestra i seleccio de mediana
			       insertSort(window);
			       median = window[WINDOW_SIZE / 2];
			       *(filteredDepthData + j + (i * matrixWidth)) = median;
				//std::cout << "Thread " << ((threadArgs*)threadData)->threadID << " treated position " << i << " " << j << "\n\n";
		       }
		}
		//Notifiquem que hem acabat, evitem tornar a entrar al bucle
		*threadStart = 0;
	}
}

int main(int argc, char *argv[]){
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//-------------SETUP DELS SENSORS DE PROFUNDITAT I IMATGE-------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
	
	//Inicialitzem dispositiu
	Status rc = OpenNI::initialize();
	if(rc != STATUS_OK) std::cout << "Error d'inicialitzacio\n\n" << OpenNI::getExtendedError();
	else std::cout << "Inicialitzat correctament\n\n";

	//Obrim dispositiu
	Device device;
	rc = device.open(ANY_DEVICE);
	if(rc != STATUS_OK) std::cout << "Error obrint dispositiu\n\n" << OpenNI::getExtendedError();
	else std::cout << "Device obert correctament\n\n";

	//Creem video stream de profunditat
	VideoStream depth;

	if(device.getSensorInfo(SENSOR_DEPTH) != NULL){
		rc = depth.create(device, SENSOR_DEPTH);
		if(rc != STATUS_OK) std::cout << "No es pot crear el stream de profunditat\n\n" << OpenNI::getExtendedError();
		else std::cout << "Stream de profunditat creat\n\n";
	}

	//Creem video stream d'imatge
	VideoStream image;

	if(device.getSensorInfo(SENSOR_COLOR) != NULL){
		rc = image.create(device, SENSOR_COLOR);
		if(rc != STATUS_OK) std::cout << "No es pot crear el stream d'imatge\n\n" << OpenNI::getExtendedError();
		else std::cout << "Stream d'imatge creat\n\n";
	}
	
	//Comencem streams
	rc = depth.start();
	if(rc != STATUS_OK) std::cout << "No es pot començar el stream de profunditat\n\n" << OpenNI::getExtendedError();
	else std::cout << "Comencem stream de profunditat\n\n";

	rc = image.start();
	if(rc != STATUS_OK) std::cout << "No es pot començar el stream d'imatge\n\n" << OpenNI::getExtendedError();
	else std::cout << "Comencem stream d'imatge\n\n";
	
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//-------------SETUP DEL SOCKET PER A TRANSMISSIO DE DADES------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//

	int socket_fd;
	struct sockaddr_in serv_addr;
	char *message = "Test message";
	char buffer[1000] = {0};
	
	//Inicialitzem socket
	socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(socket_fd < 0){
		std::cout << "Creació de socket fallida\n";
		return -1;
	}

	//Configurem struct de serv_addr
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(PORT);

	if(inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0){
		std::cout << "Direcció IP destí no valida o no suportada\n";
		return -2;
	}

	//Connexió amb el socket destí
	if(connect(socket_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0){
		std::cout << "Connexió fallida\n";
		return -3;
	}

	std::cout << "Connexió a servidor satisfactoria\n\n";

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//--------------INICI DE LOOP DE TRACTAMENT I TRANSMISSIO-------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//

	//Definició de variables i estructures d'ús per a tot el loop	
	VideoFrameRef frame, frameImage;
	int heightDepth, widthDepth, sizeInBytesDepth, strideDepth;
	int heightImage, widthImage, sizeInBytesImage, strideImage;
	
	lzo_uint inLength, lzoOutLength;

	int lzo_status;

	int sentBytes= 0;
	int networkDepthSize, networkDepthX, networkDepthY;

	//Agafem un primer frame per obtenir les seves mesures
	rc = depth.readFrame(&frame);
	heightDepth = frame.getHeight();
	widthDepth = frame.getWidth();
	
	//Creem matriu secundaria on guardar futurs filtratges i finestra de convolucio
	uint16_t filteredDepthData [heightDepth][widthDepth];

	//Inicialitzem el contingut de la matriu secundaria a un
	//valor elevat per treure l'efecte "paret" dels grafics
	for(int i = 0; i < heightDepth * widthDepth; ++i)
		*((uint16_t*)filteredDepthData + i) = WALL_OFFSET;

	//Enviem mesures del frame al servidor
	networkDepthX = htonl(widthDepth);
	networkDepthY = htonl(heightDepth);
	send(socket_fd, &networkDepthX, sizeof(networkDepthX), 0);
	send(socket_fd, &networkDepthY, sizeof(networkDepthY), 0);

	std::cout << "Enviat tamany d'eix X: " << widthDepth << " elements\n";
	std::cout << "Enviat tamany d'eix Y: " << heightDepth << " elements\n";

	//THREAD SETUP AND CREATION, ONLY USED IF WE USE MEDIAN FILTERING
	if(WINDOW_SIZE != 1){
		//Cada thread processa una tercera part de la matriu (restem el offset de cada costat)
		int heightChunkSize = (heightDepth - (WINDOW_SIDE_HALF*2))/3;
		int heightChunkRemainder = (heightDepth - (WINDOW_SIDE_HALF*2))%3;

		//Si la matriu no es divisible entre 3, ajustem carrega de cada thread
		if(heightChunkRemainder == 1){
			threadArgs1.firstRow = WINDOW_SIDE_HALF;
			threadArgs1.lastRow = WINDOW_SIDE_HALF + heightChunkSize + 1;	
			threadArgs2.firstRow = threadArgs1.lastRow + 1;
			threadArgs2.lastRow = threadArgs2.firstRow + heightChunkSize;	
			threadArgs3.firstRow = threadArgs2.lastRow + 1;
			threadArgs3.lastRow = threadArgs3.firstRow + heightChunkSize;	
		} else if(heightChunkRemainder == 2){
			threadArgs1.firstRow = WINDOW_SIDE_HALF;
			threadArgs1.lastRow = WINDOW_SIDE_HALF + heightChunkSize + 1;	
			threadArgs2.firstRow = threadArgs1.lastRow + 1;
			threadArgs2.lastRow = threadArgs2.firstRow + heightChunkSize + 1;	
			threadArgs3.firstRow = threadArgs2.lastRow + 1;
			threadArgs3.lastRow = threadArgs3.firstRow + heightChunkSize;	
		} else{
			threadArgs1.firstRow = WINDOW_SIDE_HALF;
			threadArgs1.lastRow = WINDOW_SIDE_HALF + heightChunkSize;	
			threadArgs2.firstRow = threadArgs1.lastRow + 1;
			threadArgs2.lastRow = threadArgs2.firstRow + heightChunkSize;	
			threadArgs3.firstRow = threadArgs2.lastRow + 1;
			threadArgs3.lastRow = threadArgs3.firstRow + heightChunkSize;	
		}

		//L'amplada de la matriu es la mateixa per a tots els threads
		threadArgs1.matrixWidth = threadArgs2.matrixWidth = threadArgs3.matrixWidth = widthDepth;

		threadArgs1.threadID = 1;
		threadArgs2.threadID = 2;
		threadArgs3.threadID = 3;

		threadArgs1.outputData = threadArgs2.outputData = threadArgs3.outputData = &filteredDepthData[0][0];

		std::cout << "Creant threads, direccions dels flags de cada thread: \n";
		std::cout << &thread1Start << "\n";
		std::cout << &thread2Start << "\n";
		std::cout << &thread3Start << "\n\n";
		
		pthread_create(&thread1, NULL, medianFilterWorker, (void*)&threadArgs1);
		pthread_create(&thread2, NULL, medianFilterWorker, (void*)&threadArgs2);
		pthread_create(&thread3, NULL, medianFilterWorker, (void*)&threadArgs3);
	}

	//Comencem loop
	int counter = 0;
	while(counter != 3000){
		//......................TASK 1 - FRAME PROCESSING...................//
		//Capturem un sol frame d'informació 3D i imatge
		std::cout << "--------------------------------------\n";

		rc = depth.readFrame(&frame);
		if(rc != STATUS_OK) std::cout << "No es pot fer la lectura del frame de profunditat\n\n" << OpenNI::getExtendedError();
		else std::cout << "Frame de profunditat capturat\n\n";

		rc = image.readFrame(&frameImage);
		if(rc != STATUS_OK) std::cout << "No es pot fer la lectura del frame d'imatge\n\n" << OpenNI::getExtendedError();
		else std::cout << "Frame d'imatge capturat\n\n";

		//PROFUNDITAT
		heightDepth = frame.getHeight();
		widthDepth = frame.getWidth();
		sizeInBytesDepth = frame.getDataSize();
		strideDepth = frame.getStrideInBytes();

		//Obtenim matriu d'elements i la guardem en un format CSV
		//Les dades són de 2 bytes, si fos un altre s'ha dutilitzar el uintX_t equivalen
		depthData = (uint16_t*)frame.getData();

		//IMATGE
		heightImage = frameImage.getHeight();
		widthImage = frameImage.getWidth();
		sizeInBytesImage = frameImage.getDataSize();
		strideImage = frameImage.getStrideInBytes();

		//Obtenim matriu d'elements RGB i la guardem en CSV i PPM.
		//Cada element Són 3 bytes (un per cada canal RGB).
		imageData = (uint8_t*)frameImage.getData();

		//**************************** TASK 2 - DEPTH FRAME FILTERING *************************//
		
		//Filtratge via mediana de finestra. Nomes aplica si el tamany de finestra no es trivial. 
		if(WINDOW_SIZE != 1){
			std::cout << "Iniciant filtratge per mediana amb tamany de finestra: " << WINDOW_SIDE << "x" << WINDOW_SIDE << "\n\n";

			//Posem els flags d'inici de processat per a cada thread
			thread1Start = 1;
			thread2Start = 1;
			thread3Start = 1;
			
			//DEBUG
			std::cout << "FLAG STATES BEFORE BARRIER: " << thread1Start << thread2Start << thread3Start << "\n";

			//Esperem a que acabin
			while(thread1Start || thread2Start || thread3Start);

			std::cout << "FLAG STATES AFTER BARRIER: " << thread1Start << thread2Start << thread3Start << "\n";
			std::cout << "Tots els threads han acabat, procedim\n\n";
		}

		//******************************* TASK 3 - COMPRESSING ********************************//
		//Comprimim les dades si el flag de compressio esta activat
		if(COMPRESSION){
			//Inicialitzar minilzo
			if(lzo_init() != LZO_E_OK){
				std::cout << "Error inicialitzant minilzo\n\n";
			}
			
			std::cout << "Iniciant compressió amb LZO " << lzo_version_string() << "\n\n";

			//Comprimir amb les dades obtingudes, enviar versio filtrada si es fa servir
			inLength = sizeInBytesDepth;
			if (WINDOW_SIZE != 1) lzo_status = lzo1x_1_compress((const unsigned char*) filteredDepthData, inLength, lzoArray, &lzoOutLength, wrkmem);
			else lzo_status = lzo1x_1_compress((const unsigned char*) depthData, inLength, lzoArray, &lzoOutLength, wrkmem);
			
			if (lzo_status == LZO_E_OK) 
				std::cout << "Compressió de " << inLength << " bytes a " << lzoOutLength << " bytes\n\n";
			else{
				std::cout << "ERROR: compressió fallida\n\n";
				return 2;
			}
		}


		//***********************TASK 4 - SENDING AND SOCKET MANAGEMENT************************//
		
		//Enviament del tamany del frame en bytes, en network long
		if(COMPRESSION) networkDepthSize = htonl(lzoOutLength);
		else networkDepthSize = htonl(sizeInBytesDepth);

		send(socket_fd, &networkDepthSize, sizeof(networkDepthSize), 0);

		//Enviament del frame SI ESTA COMPRIMIT
		while(sentBytes < lzoOutLength && sentBytes != -1 && COMPRESSION){
			sentBytes = send(socket_fd, (char *) lzoArray, lzoOutLength, 0);
			std::cout << "Enviats " << sentBytes << " bytes\n";
		} 

		//Enviament del frame SI NO ESTA COMPRIMIT
		while(sentBytes < sizeInBytesDepth && sentBytes != -1 && !COMPRESSION){
			if (WINDOW_SIZE != 1) sentBytes = send(socket_fd, (char *) filteredDepthData, sizeInBytesDepth, 0);
			else sentBytes = send(socket_fd, (char *) depthData, sizeInBytesDepth, 0);
			std::cout << "Enviats " << sentBytes << " bytes\n";
		} 

		if(sentBytes < 0) perror("Error enviant dades: ");
		std::cout << "Frame " << counter << " enviat\n";
		sentBytes = 0;
		++counter;
	
	}
	std::cout << "--------------------------------------\n\n";


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
//-----------------------SHUTDOWN DEL PROGRAMA------------------------//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++//
	//Tanquem dispositius i fem shutdown
	std::cout << "Fent release del frame de profunditat\n\n";
	frame.release();
	std::cout << "Fent release del frame d'imatge\n\n";
	frameImage.release();
	std::cout << "Parant stream de profunditat\n\n";
	depth.stop();
	std::cout << "Parant stream d'imatge\n\n";
	image.stop();
	std::cout << "Eliminant stream object de profunditat\n\n";
	depth.destroy();
	std::cout << "Eliminant stream object d'imatge\n\n";
	image.destroy();
	std::cout << "Tancant dispositiu\n\n";
	device.close();
	std::cout << "Fent shutdown\n\n";
	OpenNI::shutdown();
	std::cout << "FINALITZAT\n\n";

	return 0;
}
