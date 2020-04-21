// MapGeneratorMT.cpp : Generate a text file containing a "map" for testing path finding
//
// The format of the text file is:
// <dimension>
// . . . . @ @ @ @ . . .
// . . . . @ @ @ @ . . .
// . . . . . . . . . . .
//
// The dot character specifies an "open" cell.
// The @ character specifies an "obstacle" or "wall" cell
//
//
#include <Windows.h>
#include <iostream>
#include <sstream>
#include <string>
#include <time.h>

#define USAGE "MapGenerator <dimension> <num_obstacles> <obstacle_max_size> <num_threads> <scale_factor> <seed>\n\n"

#define OUTPUT_FILENAME "./map.txt"

#define MAX(a, b) (a > b ? a : b)
#define MIN(a, b) (a < b ? a : b)

const char cOPEN_CHAR = '.';
const char cOBSTACLE_CHAR = '@';

using namespace std;

bool initializeMap(char** pcMap, int iDimensionRows, int iDimensionCols);
DWORD WINAPI addObstacle(LPVOID lpParam);
DWORD WINAPI printMap(LPVOID lpParam);
DWORD WINAPI printMapScaled(LPVOID lpParam);
bool combineMapFiles(FILE* pFile, int iDimension, int iScaleFactor);

// for bitmap output
const int bytesPerPixel = 3; /// red, green, blue
const int fileHeaderSize = 14;
const int infoHeaderSize = 40;
void generateBitmapImage(unsigned char *image, int height, int width, char* imageFileName);
unsigned char* createBitmapFileHeader(int height, int width, int paddingSize);
unsigned char* createBitmapInfoHeader(int height, int width);
bool createBitmap(char** pcMap, int iDimensionRows, int iDimensionCols, char* imageFileName);

typedef struct _OBSTACLE_THREAD_ARGS
{
	char** ppcMap;
	int iDimensionRows;
	int iDimensionCols;
	int iObstacleMaxSize;
} OBSTACLE_THREAD_ARGS;

typedef struct _FILE_WRITE_ARGS
{
	char** ppcMap;
	int iDimensionRows;
	int iDimensionCols;
	const char* pszFilename;
	int iSuffix;
	int iStartLine;
	int iEndLine;
	int iScaleFactor;
} FILE_WRITE_ARGS;

HANDLE ghMapMutex;
HANDLE ghObstacleMutex;
int giNumObstaclesRemaining;
int giNumThreads = 1;

/*-----------------------------------------------
	
-------------------------------------------------*/
int main(int argc, char* argv[])
{
	time_t tStart = time(0);
	time_t tEnd;

	if (argc != 7)
	{
		printf(USAGE);
		return 1;
	}

	int iDimension = atoi(argv[1]);
	if (iDimension <= 0)
	{
		printf("The input dimension, %s, is not valid. It has to be repeatedly divisible by 2.\n", argv[1]);
		printf(USAGE);
		return 1;
	}

	int iNumObstacles = atoi(argv[2]);
	if (iNumObstacles <= 0)
	{
		printf("The number of obstacles, %s, is not valid.\n", argv[2]);
		printf(USAGE);
		return 1;
	}
	giNumObstaclesRemaining = iNumObstacles;

	int iObstacleMaxSize = atoi(argv[3]);
	if (iObstacleMaxSize <= 0 || iObstacleMaxSize >= iDimension)
	{
		printf("The max size of obstacles, %s, is not valid.\n", argv[3]);
		printf(USAGE);
		return 1;
	}

	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	giNumThreads = sysinfo.dwNumberOfProcessors;

	giNumThreads = atoi(argv[4]);
	if (giNumThreads <= 0)
	{
		printf("The number of threads, %s, is not valid.\n", argv[4]);
		printf(USAGE);
		return 1;
	}
	if (giNumThreads > (int) sysinfo.dwNumberOfProcessors)
	{
		printf("Warning: The number of threads, %s, is more than the number of processors on this machine (%d).\n",
			argv[4], sysinfo.dwNumberOfProcessors);
	}

	int iScaleFactor = atoi(argv[5]);
	if (iScaleFactor < 1)
	{
		printf("The scale factor, %s, is not valid.\n", argv[5]);
		printf(USAGE);
		return 1;
	}

	int iSeed = atoi(argv[6]);
	if (iSeed < 0)
	{
		printf("The seed, %s, is not valid.\n", argv[6]);
		printf(USAGE);
		return 1;
	}

	int iResult = iDimension * iScaleFactor;
	int iRemainder;
	bool bContinue = true;
	bool bValid = false;

	while (bContinue)
	{
		iRemainder = iResult % 2;
		if (!iRemainder) // divisible by 2
		{
			iResult = iResult / 2;
		}
		else
		{
			break;
		}
		if (iResult == 1)
		{
			bValid = true;
			break;
		}
	}

	if (!bValid)
	{
		printf("The input dimension and scale factor combined, %d * %d, is not valid.\n",
			iDimension, iScaleFactor);
		return 1;
	}

	// use the user-supplied seed if given
	if (iSeed == 0)
	{
		iSeed = (int)time(0);
	}
	srand((unsigned int)iSeed);

	// print the arguments
	fprintf(stdout, "\n");
	fprintf(stdout, "Dimension: %d\n", iDimension);
	fprintf(stdout, "Obstacles: %d\n", iNumObstacles);
	fprintf(stdout, "Obstacle Max Size: %d x %d\n", iObstacleMaxSize, iObstacleMaxSize);
	fprintf(stdout, "Number of Threads: %d\n", giNumThreads);
	fprintf(stdout, "Scale Factor: %d\n", iScaleFactor);
	fprintf(stdout, "Seed: %d\n", iSeed);
	fprintf(stdout, "Final Dimension: %d x %d\n", (iDimension * iScaleFactor), (iDimension * iScaleFactor));
	fprintf(stdout, "\n");

	// make sure the output file doesn't already exist
	struct stat statbuf;
	if (stat(OUTPUT_FILENAME, &statbuf) == 0)
	{
		//fprintf(stdout, "The file %s already exists.  Remove it and rerun.\n", OUTPUT_FILENAME);
		//return 1;
		fprintf(stdout, "The file %s already exists.  It will be deleted.\n", OUTPUT_FILENAME);
	}

	FILE* pfOutputFile;
	if ((fopen_s(&pfOutputFile, OUTPUT_FILENAME, "w")) != 0)
	{
		fprintf(stdout, "Unable to open the output map file for writing: %s\n", OUTPUT_FILENAME);
		return 1;
	}

	char* pcMap = NULL;
	if (!initializeMap(&pcMap, iDimension, iDimension))
	{
		fprintf(stdout, "Unable to create/initialize the map of size %d\n", iDimension);
		return 1;
	}

	HANDLE* phThreads = new HANDLE [giNumThreads];
	if (!phThreads)
	{
		fprintf(stdout, "Can't allocate memory\n");
		return 1;
	}

	FILE_WRITE_ARGS** fileargs = new FILE_WRITE_ARGS* [giNumThreads];
	if (!fileargs)
	{
		fprintf(stdout, "Can't allocate memory\n");
		return 1;
	}
	else
	{
		for (int i = 0; i < giNumThreads; i++)
		{
			fileargs[i] = new FILE_WRITE_ARGS;
		}
	}

	ghMapMutex = CreateMutex(NULL, FALSE, NULL);
	if (ghMapMutex == NULL)
	{
		printf("CreateMutex error: %d\n", GetLastError());
		return 1;
	}
	ghObstacleMutex = CreateMutex(NULL, FALSE, NULL);
	if (ghObstacleMutex == NULL)
	{
		printf("CreateMutex error: %d\n", GetLastError());
		return 1;
	}

	OBSTACLE_THREAD_ARGS args;
	args.ppcMap = &pcMap;
	args.iDimensionCols = iDimension;
	args.iDimensionRows = iDimension;
	args.iObstacleMaxSize = iObstacleMaxSize;
	addObstacle(&args);

	// start threads to write out sections of the map
	DWORD dwThreadId;
	int iLinesPerFile = iDimension / giNumThreads;
	int iRemainingLines = iDimension - (iLinesPerFile * giNumThreads);
	for (int i = 0; i < giNumThreads; i++)
	{
		fileargs[i]->ppcMap = &pcMap;
		fileargs[i]->iDimensionCols = iDimension;
		fileargs[i]->iDimensionRows = iDimension;
		fileargs[i]->pszFilename = OUTPUT_FILENAME;
		fileargs[i]->iSuffix = i;
		fileargs[i]->iStartLine = i * iLinesPerFile;
		fileargs[i]->iEndLine = fileargs[i]->iStartLine + iLinesPerFile;
		if (i + 1 >= giNumThreads)
		{
			fileargs[i]->iEndLine += iRemainingLines;
		}
		fileargs[i]->iScaleFactor = iScaleFactor;

		phThreads[i] = CreateThread(
			NULL,       // default security attributes
			0,          // default stack size
			(LPTHREAD_START_ROUTINE)printMapScaled,
			fileargs[i],       // thread function arguments
			0,          // default creation flags
			&dwThreadId); // receive thread identifier

		if (phThreads[i] == NULL)
		{
			printf("CreateThread error: %d\n", GetLastError());
			return 1;
		}
		fprintf(stdout, "Started thread id %d\n", dwThreadId);
	}

	// wait for all print threads to complete
	WaitForMultipleObjects(giNumThreads, phThreads, TRUE, INFINITE);

	for (int i = 0; i < giNumThreads; i++)
	{
		CloseHandle(phThreads[i]);
	}
	if (phThreads)
	{
		delete[] phThreads;
	}

	// combine the separate map files into one
	combineMapFiles(pfOutputFile, iDimension, iScaleFactor);

	if (pfOutputFile != NULL)
	{
		fclose(pfOutputFile);
	}

	// create a bitmap image of the map.  it will be upside down
	createBitmap(&pcMap, iDimension, iDimension, (char*) "./image.bmp");

	// cleanup
	if (pcMap)
	{
		delete [] pcMap;
	}

	CloseHandle(ghMapMutex);
	CloseHandle(ghObstacleMutex);

	for (int i = 0; i < giNumThreads; i++)
	{
		if (fileargs[i])
		{
			delete fileargs[i];
		}
	}
	delete[] fileargs;

	tEnd = time(0);
	int iElapsedSeconds = tEnd - tStart;
	int iMinutes = iElapsedSeconds / 60;
	int iRemaining = iElapsedSeconds - (60 * iMinutes);
	fprintf(stdout, "Elapsed time: %d min %d sec\n", iMinutes, iRemaining);

	printf("Press RETURN to continue...");
	getc(stdin);
}

/*-----------------------------------------------
	Initialize the map to all open
-------------------------------------------------*/
bool initializeMap(char** ppcMap, int iDimensionRows, int iDimensionCols)
{
	bool bRc = false;
	if (!ppcMap || iDimensionRows <= 0 || iDimensionCols <= 0)
	{
		return false;
	}

	//fprintf(stdout, "Initializing a map of size %d x %d\n", iDimensionRows, iDimensionCols);

	// initialize the map to all 0s
	char* pMap = new char [iDimensionRows * iDimensionCols];
	if (pMap != NULL)
	{
		//memset(piMap, 0, iDimensionRows * iDimensionCols * sizeof(int));

		for (int i = 0; i < iDimensionRows; i++) // for each row
		{
			for (int j = 0; j < iDimensionCols; j++) // for each column
			{
				*(pMap + i * iDimensionCols + j) = cOPEN_CHAR;
			}
		}
		bRc = true;
	}
	*ppcMap = pMap;
	return bRc;
}

/*-----------------------------------------------
	Write out a range of lines of the map to a file
-------------------------------------------------*/
DWORD WINAPI printMap(LPVOID lpParam)
{
	FILE_WRITE_ARGS* args = (FILE_WRITE_ARGS*)lpParam;
	bool bContinue = true;

	if (args->pszFilename == NULL || (args->iEndLine < args->iStartLine) || args->iSuffix < 0 ||
		!args->ppcMap || args->iDimensionRows <= 0 || args->iDimensionCols <= 0)
	{
		return false;
	}


	fprintf(stdout, "Thread id %d Writing to map file %d to %d...\n", 
		GetCurrentThreadId(), args->iStartLine, args->iEndLine);

	char szFilename[MAX_PATH];
	sprintf_s(szFilename, "%s.%d", args->pszFilename, args->iSuffix);
	FILE* pFile = NULL;
	if ((fopen_s(&pFile, szFilename, "w")) != 0)
	{
		fprintf(stdout, "Thread id %d Unable to open the output map file for writing: %s\n",
			GetCurrentThreadId(), szFilename);
		return 1;
	}

	int iMaxLineChars = args->iDimensionRows * 2 + 16; // *2 for the char plus a space, + 16 for a row header
	char* pszLine = new char[iMaxLineChars];
	if (pszLine == NULL)
	{
		return 1;
	}
	pszLine[0] = '\0';

	char szChars[3];
	for (int i = args->iStartLine; i < args->iEndLine; i++) // for each row
	{
		if (i % 500 == 0)
		{
			fprintf(stdout, "Thread id %d Starting to print row %d\n", GetCurrentThreadId(), i);
		}

		pszLine[0] = '\0';
		for (int j = 0; j < args->iDimensionCols; j++) // for each column
		{
			sprintf_s(szChars, sizeof(szChars), "%1c ", *(*args->ppcMap + i * args->iDimensionRows + j));
			strcat_s(pszLine, iMaxLineChars, szChars);
		}

		fprintf(pFile, "%s\n", pszLine);
	}

	fclose(pFile);

	delete[] pszLine;

	delete args;

	return 0;
}

/*-----------------------------------------------
	Scale and write out a range of lines of the map to a file.
-------------------------------------------------*/
DWORD WINAPI printMapScaled(LPVOID lpParam)
{
	FILE_WRITE_ARGS* args = (FILE_WRITE_ARGS*)lpParam;
	bool bContinue = true;

	if (args->pszFilename == NULL || (args->iEndLine < args->iStartLine) || args->iSuffix < 0 ||
		!args->ppcMap || args->iDimensionRows <= 0 || args->iDimensionCols <= 0)
	{
		return false;
	}


	fprintf(stdout, "Thread id %d Writing to map file %d to %d, scale factor %d...\n",
		GetCurrentThreadId(), args->iStartLine, args->iEndLine, args->iScaleFactor);

	char szFilename[MAX_PATH];
	sprintf_s(szFilename, "%s.%d", args->pszFilename, args->iSuffix);
	FILE* pFile = NULL;
	if ((fopen_s(&pFile, szFilename, "w")) != 0)
	{
		fprintf(stdout, "Thread id %d Unable to open the output map file for writing: %s\n",
			GetCurrentThreadId(), szFilename);
		return 1;
	}

	int iMaxLineChars = args->iDimensionRows * 2 * args->iScaleFactor + 16; // *2 for the char plus a space, + 16 for a row header
	char* pszLine = new char[iMaxLineChars];
	if (pszLine == NULL)
	{
		return 1;
	}
	pszLine[0] = '\0';

	int iLinesWritten = 0;
	char szChars[3];
	for (int i = args->iStartLine; i < args->iEndLine; i++) // for each row
	{
		pszLine[0] = '\0';
		for (int j = 0; j < args->iDimensionCols; j++) // for each column
		{
			sprintf_s(szChars, sizeof(szChars), "%1c ", *(*args->ppcMap + i * args->iDimensionRows + j));
			for (int k = 0; k < args->iScaleFactor; k++)
			{
				strcat_s(pszLine, iMaxLineChars, szChars);
			}
		}

		for (int k = 0; k < args->iScaleFactor; k++)
		{
			fprintf(pFile, "%s\n", pszLine);
			iLinesWritten++;
			if (iLinesWritten % 500 == 0)
			{
				fprintf(stdout, "Thread id %d has printed %d lines\n", GetCurrentThreadId(), iLinesWritten);
			}
		}
	}

	fclose(pFile);

	delete[] pszLine;

	return 0;
}

/*-----------------------------------------------
	Combine the individual map files created by the
	threads into a single file.
-------------------------------------------------*/
bool combineMapFiles(FILE* pFile, int iDimension, int iScaleFactor)
{
	bool bRc = false;
#define CHUNK_SIZE 65536
	unsigned char ucaBuffer[CHUNK_SIZE];

	if (pFile == NULL)
	{
		return false;
	}

	fprintf(stdout, "Combining separate maps files into %s\n", OUTPUT_FILENAME);

	// first write out the dimension
	fprintf(pFile, "%d\n", iDimension * iScaleFactor);

	char szFilename[MAX_PATH];
	for (int i = 0; i < giNumThreads; i++)
	{
		sprintf_s(szFilename, MAX_PATH, "%s.%d", OUTPUT_FILENAME, i);
		FILE* pInput;
		if (fopen_s(&pInput, szFilename, "r") != 0)
		{
			fprintf(stdout, "Failed to open one of the map files: %s\n", szFilename);
			return false;
		}

		size_t nTotalRead = 0;
		size_t nTotalWritten = 0;
		size_t nCharsWritten;
		size_t nCharsRead = fread_s(ucaBuffer, CHUNK_SIZE, sizeof(unsigned char), CHUNK_SIZE, pInput);
		while (nCharsRead > 0)
		{
			nTotalRead += nCharsRead;
			nCharsWritten = fwrite(ucaBuffer, sizeof(unsigned char), nCharsRead, pFile);
			nTotalWritten += nCharsWritten;
			nCharsRead = fread_s(ucaBuffer, CHUNK_SIZE, sizeof(unsigned char), CHUNK_SIZE, pInput);
		}
		fclose(pInput);
		//fprintf(stdout, "%s - %ld chars read, %ld chars written\n", szFilename, (long) nTotalRead, (long) nTotalWritten);
		fprintf(stdout, "Deleting temporary file %s\n", szFilename);
		remove(szFilename);
	}

	return bRc;
}


/*-----------------------------------------------
	Add an obstacle/wall to the map.  
	Returns true if the obstacle was added.
-------------------------------------------------*/
DWORD WINAPI addObstacle(LPVOID lpParam)
{
	OBSTACLE_THREAD_ARGS* args = (OBSTACLE_THREAD_ARGS*) lpParam;
	bool bContinue = true;

	if (!args->ppcMap || args->iDimensionRows <= 0 || args->iDimensionCols <= 0)
	{
		return false;
	}

	fprintf(stdout, "Thread id %d Generating obstacles of max size %d x %d\n",
		GetCurrentThreadId(), args->iObstacleMaxSize, args->iObstacleMaxSize);

	while (bContinue)
	{
		DWORD dwWaitResult = WaitForSingleObject(ghObstacleMutex, INFINITE);
		if (dwWaitResult == WAIT_OBJECT_0)
		{
			if (giNumObstaclesRemaining > 0)
			{
				giNumObstaclesRemaining--;
				//fprintf(stdout, "%d obstacles remaining\n", giNumObstaclesRemaining);
			}
			else
			{
				bContinue = false;
			}
		}
		else
		{
			bContinue = false;
		}
		ReleaseMutex(ghObstacleMutex);

		if (!bContinue)
		{
			return 1;
		}

		//fprintf(stdout, "Thread id %d is working\n", GetCurrentThreadId());

		// pick a random width and height > 0 for the new obstacle
		int iWidth;
		int iHeight;
		while (true)
		{
			iWidth = rand() % args->iObstacleMaxSize;
			if (iWidth > 0)
			{
				break;
			}
		}
		while (true)
		{
			iHeight = rand() % args->iObstacleMaxSize;
			if (iHeight > 0)
			{
				break;
			}
		}

		// pick a random row and column > 0 and < max for the new obstacle
		int iRow;
		int iCol;
		while (true)
		{
			iRow = rand() % (args->iDimensionRows - 1);
			if (iRow > 0)
			{
				break;
			}
		}
		while (true)
		{
			iCol = rand() % (args->iDimensionCols - 1);
			if (iCol > 0)
			{
				break;
			}
		}

		//fprintf(stdout, "Thread id %d Obstacle at row %d, col %d of width %d, height %d\n", 
		//	GetCurrentThreadId(), iRow, iCol, iWidth, iHeight);

		int iEndRow = MIN((iRow + iHeight), args->iDimensionRows);
		int iEndCol = MIN((iCol + iWidth), args->iDimensionCols);

		dwWaitResult = WaitForSingleObject(ghMapMutex, INFINITE);
		if (dwWaitResult == WAIT_OBJECT_0)
		{
			for (int i = iRow; i < iEndRow; i++)
			{
				for (int j = iCol; j < iEndCol; j++)
				{
					*(*args->ppcMap + i * args->iDimensionCols + j) = cOBSTACLE_CHAR;
				}
			}
		}
		ReleaseMutex(ghMapMutex);
	}

	return 0;
}



/*-----------------------------------------------

-------------------------------------------------*/
bool createBitmap(char** pcMap, int iDimensionRows, int iDimensionCols, char* imageFileName)
{
	fprintf(stdout, "Creating a bitmap image\n");

	unsigned char* pImage = new unsigned char[iDimensionRows * iDimensionCols * bytesPerPixel];

	int iVal;
	int i, j;
	for (i = 0; i < iDimensionRows; i++)
	{
		for (j = 0; j < iDimensionCols; j++)
		{
			iVal = *(*pcMap + i * iDimensionCols + j) == cOPEN_CHAR ? 255 : 0;

			*(pImage + i*iDimensionCols*bytesPerPixel + j*bytesPerPixel + 2) = (unsigned char)(iVal); ///red
			*(pImage + i*iDimensionCols*bytesPerPixel + j*bytesPerPixel + 1) = (unsigned char)(iVal); ///green
			*(pImage + i*iDimensionCols*bytesPerPixel + j*bytesPerPixel + 0) = (unsigned char)(iVal); ///blue
		}
	}

	generateBitmapImage(pImage, iDimensionRows, iDimensionCols, imageFileName);
	printf("Image generated: %s\n", imageFileName);

	delete[] pImage;
	return true;
}

/*-----------------------------------------------

-------------------------------------------------*/
void generateBitmapImage(unsigned char *image, int height, int width, char* imageFileName)
{

	unsigned char padding[3] = { 0, 0, 0 };
	int paddingSize = (4 - (width*bytesPerPixel) % 4) % 4;

	unsigned char* fileHeader = createBitmapFileHeader(height, width, paddingSize);
	unsigned char* infoHeader = createBitmapInfoHeader(height, width);

	FILE* imageFile;
	if (fopen_s(&imageFile, imageFileName, "wb") != 0)
	{
		fprintf(stdout, "Couldn't open bitmap file %s\n", imageFileName);
		return;
	}

	fwrite(fileHeader, 1, fileHeaderSize, imageFile);
	fwrite(infoHeader, 1, infoHeaderSize, imageFile);

	int i;
	for (i = height - 1; i >= 0; i--)
	{
		fwrite(image + (i*width*bytesPerPixel), bytesPerPixel, width, imageFile);
		fwrite(padding, 1, paddingSize, imageFile);
	}

	fclose(imageFile);
}

/*-----------------------------------------------

-------------------------------------------------*/
unsigned char* createBitmapFileHeader(int height, int width, int paddingSize)
{
	int fileSize = fileHeaderSize + infoHeaderSize + (bytesPerPixel*width + paddingSize) * height;

	static unsigned char fileHeader[] = {
		0,0, /// signature
		0,0,0,0, /// image file size in bytes
		0,0,0,0, /// reserved
		0,0,0,0, /// start of pixel array
	};

	fileHeader[0] = (unsigned char)('B');
	fileHeader[1] = (unsigned char)('M');
	fileHeader[2] = (unsigned char)(fileSize);
	fileHeader[3] = (unsigned char)(fileSize >> 8);
	fileHeader[4] = (unsigned char)(fileSize >> 16);
	fileHeader[5] = (unsigned char)(fileSize >> 24);
	fileHeader[10] = (unsigned char)(fileHeaderSize + infoHeaderSize);

	return fileHeader;
}

/*-----------------------------------------------

-------------------------------------------------*/
unsigned char* createBitmapInfoHeader(int height, int width)
{
	static unsigned char infoHeader[] = {
		0,0,0,0, /// header size
		0,0,0,0, /// image width
		0,0,0,0, /// image height
		0,0, /// number of color planes
		0,0, /// bits per pixel
		0,0,0,0, /// compression
		0,0,0,0, /// image size
		0,0,0,0, /// horizontal resolution
		0,0,0,0, /// vertical resolution
		0,0,0,0, /// colors in color table
		0,0,0,0, /// important color count
	};

	infoHeader[0] = (unsigned char)(infoHeaderSize);
	infoHeader[4] = (unsigned char)(width);
	infoHeader[5] = (unsigned char)(width >> 8);
	infoHeader[6] = (unsigned char)(width >> 16);
	infoHeader[7] = (unsigned char)(width >> 24);
	infoHeader[8] = (unsigned char)(height);
	infoHeader[9] = (unsigned char)(height >> 8);
	infoHeader[10] = (unsigned char)(height >> 16);
	infoHeader[11] = (unsigned char)(height >> 24);
	infoHeader[12] = (unsigned char)(1);
	infoHeader[14] = (unsigned char)(bytesPerPixel * 8);

	return infoHeader;
}