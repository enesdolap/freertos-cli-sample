/*
 * CommandLineInterface.c
 *
 *  Created on: Apr 20, 2022
 *      Author: enes.dolap
 */

#include "CommandLineInterface.h"

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* FreeRTOS+CLI includes. */
#include "FreeRTOS_CLI.h"

#include "stm32f7xx_hal.h"

/* Dimensions the buffer into which input characters are placed. */
#define cmdMAX_INPUT_SIZE		50

/* The maximum time in ticks to wait for the UART access mutex. */
#define cmdMAX_MUTEX_WAIT		( 200 / portTICK_PERIOD_MS )

/* Characters are only ever received slowly on the CLI so it is ok to pass
received characters from the UART interrupt to the task on a queue.  This sets
the length of the queue used for that purpose. */
#define cmdRXED_CHARS_QUEUE_LENGTH			( 10 )

/* DEL acts as a backspace. */
#define cmdASCII_DEL		( 0x7F )

/* Const messages output by the command console. */
static char * const pcWelcomeMessage = "\r\n\r\nFreeRTOS command server.\r\nType Help to view a list of registered commands.\r\n\r\n>";
static const char * const pcEndOfOutputMessage = "\r\n[Press ENTER to execute the previous command again]\r\n>";
static const char * const pcNewLine = "\r\n";

extern UART_HandleTypeDef huart3;

/* This semaphore is used to allow the task to wait for a Tx to complete
without wasting any CPU time. */
static SemaphoreHandle_t xTxCompleteSemaphore = NULL;

/* This semaphore is sued to allow the task to wait for an Rx to complete
without wasting any CPU time. */
static SemaphoreHandle_t xRxCompleteSemaphore = NULL;

static void prvUARTCommandConsoleTask( void *pvParameters );

/*
 * Implements the run-time-stats command.
 */
static portBASE_TYPE prvTaskStatsCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );

/*
 * Implements the task-stats command.
 */
static portBASE_TYPE prvRunTimeStatsCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );

/*
 * Implements the echo-three-parameters command.
 */
static portBASE_TYPE prvThreeParameterEchoCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );

/*
 * Implements the echo-parameters command.
 */
static portBASE_TYPE prvParameterEchoCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString );


/* Structure that defines the "run-time-stats" command line command.   This
generates a table that shows how much run time each task has */
static const CLI_Command_Definition_t xRunTimeStats =
{
	"run-time-stats", /* The command string to type. */
	"\r\nrun-time-stats:\r\n Displays a table showing how much processing time each FreeRTOS task has used\r\n",
	prvRunTimeStatsCommand, /* The function to run. */
	0 /* No parameters are expected. */
};

/* Structure that defines the "task-stats" command line command.  This generates
a table that gives information on each task in the system. */
static const CLI_Command_Definition_t xTaskStats =
{
	"task-stats", /* The command string to type. */
	"\r\ntask-stats:\r\n Displays a table showing the state of each FreeRTOS task\r\n",
	prvTaskStatsCommand, /* The function to run. */
	0 /* No parameters are expected. */
};

/* Structure that defines the "echo_3_parameters" command line command.  This
takes exactly three parameters that the command simply echos back one at a
time. */
static const CLI_Command_Definition_t xThreeParameterEcho =
{
	"echo-3-parameters",
	"\r\necho-3-parameters <param1> <param2> <param3>:\r\n Expects three parameters, echos each in turn\r\n",
	prvThreeParameterEchoCommand, /* The function to run. */
	3 /* Three parameters are expected, which can take any value. */
};

/* Structure that defines the "echo_parameters" command line command.  This
takes a variable number of parameters that the command simply echos back one at
a time. */
static const CLI_Command_Definition_t xParameterEcho =
{
	"echo-parameters",
	"\r\necho-parameters <...>:\r\n Take variable number of parameters, echos each in turn\r\n",
	prvParameterEchoCommand, /* The function to run. */
	-1 /* The user can enter any number of commands. */
};

static portBASE_TYPE prvTaskStatsCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString )
{
	const char *const pcHeader =
			"Task          State  Priority  Stack	#\r\n************************************************\r\n";
	// Make sure the write buffer does not contain a string.

	/* Remove compile time warnings about unused parameters, and check the
	 write buffer is not NULL.  NOTE - for simplicity, this example assumes the
	 write buffer length is adequate, so does not check for buffer overflows. */
	(void) pcCommandString;
	(void) xWriteBufferLen;

	/* Generate a table of task stats. */
	strcpy( pcWriteBuffer, pcHeader );
	vTaskList( pcWriteBuffer + strlen( pcHeader ) );

	/* There is no more data to return after this single string, so return
	 pdFALSE. */
	return pdFALSE;
}

static portBASE_TYPE prvRunTimeStatsCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString )
{
	const char * const pcHeader = "Task            Abs Time      % Time\r\n****************************************\r\n";
	/* Remove compile time warnings about unused parameters, and check the
	write buffer is not NULL.  NOTE - for simplicity, this example assumes the
	write buffer length is adequate, so does not check for buffer overflows. */
	( void ) pcCommandString;
	( void ) xWriteBufferLen;
	configASSERT( pcWriteBuffer );

	/* Generate a table of task stats. */
	strcpy( pcWriteBuffer, pcHeader );
	vTaskGetRunTimeStats( pcWriteBuffer + strlen( pcHeader ) );

	/* There is no more data to return after this single string, so return
	pdFALSE. */
	return pdFALSE;
}

static portBASE_TYPE prvThreeParameterEchoCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString )
{
	const char *pcParameter;
	portBASE_TYPE xParameterStringLength, xReturn;
	static portBASE_TYPE lParameterNumber = 0;

	/* Remove compile time warnings about unused parameters, and check the
	write buffer is not NULL.  NOTE - for simplicity, this example assumes the
	write buffer length is adequate, so does not check for buffer overflows. */
	( void ) pcCommandString;
	( void ) xWriteBufferLen;
	configASSERT( pcWriteBuffer );

	if( lParameterNumber == 0 )
	{
		/* The first time the function is called after the command has been
		entered just a header string is returned. */
		sprintf( pcWriteBuffer, "The three parameters were:\r\n" );

		/* Next time the function is called the first parameter will be echoed
		back. */
		lParameterNumber = 1L;

		/* There is more data to be returned as no parameters have been echoed
		back yet. */
		xReturn = pdPASS;
	}
	else
	{
		/* Obtain the parameter string. */
		pcParameter = FreeRTOS_CLIGetParameter
							(
								pcCommandString,		/* The command string itself. */
								lParameterNumber,		/* Return the next parameter. */
								&xParameterStringLength	/* Store the parameter string length. */
							);

		/* Sanity check something was returned. */
		configASSERT( pcParameter );

		/* Return the parameter string. */
		memset( pcWriteBuffer, 0x00, xWriteBufferLen );
		sprintf( pcWriteBuffer, "%d: ", ( int ) lParameterNumber );
		strncat( pcWriteBuffer, pcParameter, xParameterStringLength );
		strncat( pcWriteBuffer, "\r\n", strlen( "\r\n" ) );

		/* If this is the last of the three parameters then there are no more
		strings to return after this one. */
		if( lParameterNumber == 3L )
		{
			/* If this is the last of the three parameters then there are no more
			strings to return after this one. */
			xReturn = pdFALSE;
			lParameterNumber = 0L;
		}
		else
		{
			/* There are more parameters to return after this one. */
			xReturn = pdTRUE;
			lParameterNumber++;
		}
	}

	return xReturn;
}

static portBASE_TYPE prvParameterEchoCommand( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString )
{
	const char *pcParameter;
	portBASE_TYPE xParameterStringLength, xReturn;
	static portBASE_TYPE lParameterNumber = 0;

	/* Remove compile time warnings about unused parameters, and check the
	write buffer is not NULL.  NOTE - for simplicity, this example assumes the
	write buffer length is adequate, so does not check for buffer overflows. */
	( void ) pcCommandString;
	( void ) xWriteBufferLen;
	configASSERT( pcWriteBuffer );

	if( lParameterNumber == 0 )
	{
		/* The first time the function is called after the command has been
		entered just a header string is returned. */
		sprintf( pcWriteBuffer, "The parameters were:\r\n" );

		/* Next time the function is called the first parameter will be echoed
		back. */
		lParameterNumber = 1L;

		/* There is more data to be returned as no parameters have been echoed
		back yet. */
		xReturn = pdPASS;
	}
	else
	{
		/* Obtain the parameter string. */
		pcParameter = FreeRTOS_CLIGetParameter
							(
								pcCommandString,		/* The command string itself. */
								lParameterNumber,		/* Return the next parameter. */
								&xParameterStringLength	/* Store the parameter string length. */
							);

		if( pcParameter != NULL )
		{
			/* Return the parameter string. */
			memset( pcWriteBuffer, 0x00, xWriteBufferLen );
			sprintf( pcWriteBuffer, "%d: ", ( int ) lParameterNumber );
			strncat( pcWriteBuffer, pcParameter, xParameterStringLength );
			strncat( pcWriteBuffer, "\r\n", strlen( "\r\n" ) );

			/* There might be more parameters to return after this one. */
			xReturn = pdTRUE;
			lParameterNumber++;
		}
		else
		{
			/* No more parameters were found.  Make sure the write buffer does
			not contain a valid string. */
			pcWriteBuffer[ 0 ] = 0x00;

			/* No more data to return. */
			xReturn = pdFALSE;

			/* Start over the next time this command is executed. */
			lParameterNumber = 0;
		}
	}

	return xReturn;
}

void CommandLineInterfaceStart( uint16_t usStackSize, unsigned portBASE_TYPE uxPriority )
{
	FreeRTOS_CLIRegisterCommand( &xTaskStats );
	FreeRTOS_CLIRegisterCommand( &xRunTimeStats );
	FreeRTOS_CLIRegisterCommand( &xThreeParameterEcho );
	FreeRTOS_CLIRegisterCommand( &xParameterEcho );

	/* Create that task that handles the console itself. */
	xTaskCreate( 	prvUARTCommandConsoleTask,			/* The task that implements the command console. */
					"CLI",								/* Text name assigned to the task.  This is just to assist debugging.  The kernel does not use this name itself. */
					usStackSize,						/* The size of the stack allocated to the task. */
					NULL,								/* The parameter is not used, so NULL is passed. */
					uxPriority,							/* The priority allocated to the task. */
					NULL );								/* A handle is not required, so just pass NULL. */
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;

	if (huart->Instance == USART3)
	{
		/* Give the semaphore  to unblock any tasks that might be waiting for an Rx
		to complete.  If a task is unblocked, and the unblocked task has a priority
		above the currently running task, then xHigherPriorityTaskWoken will be set
		to pdTRUE inside the xSemaphoreGiveFromISR() function. */
		xSemaphoreGiveFromISR( xRxCompleteSemaphore, &xHigherPriorityTaskWoken );

		/* portEND_SWITCHING_ISR() or portYIELD_FROM_ISR() can be used here. */
		portYIELD_FROM_ISR( xHigherPriorityTaskWoken );

	}
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;

	if (huart->Instance == USART3)
	{
		/* Give the semaphore  to unblock any tasks that might be waiting for a Tx
		to complete.  If a task is unblocked, and the unblocked task has a priority
		above the currently running task, then xHigherPriorityTaskWoken will be set
		to pdTRUE inside the xSemaphoreGiveFromISR() function. */
		xSemaphoreGiveFromISR( xTxCompleteSemaphore, &xHigherPriorityTaskWoken );

		/* portEND_SWITCHING_ISR() or portYIELD_FROM_ISR() can be used here. */
		portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
	}
}

static void prvSendBuffer(const char * pcBuffer, size_t xBufferLength )
{
	const TickType_t xBlockMax100ms = 100UL / portTICK_PERIOD_MS;

	if( xBufferLength > 0 )
	{
		HAL_UART_Transmit_IT(&huart3, (uint8_t*)pcBuffer, xBufferLength);
		/* Wait for the Tx to complete so the buffer can be reused without
		corrupting the data that is being sent. */
		xSemaphoreTake( xTxCompleteSemaphore, xBlockMax100ms );
	}
}

static void prvUARTCommandConsoleTask( void *pvParameters )
{
	char cRxedChar, *pcOutputString;
	uint8_t ucInputIndex = 0;
	static char cInputString[cmdMAX_INPUT_SIZE], cLastInputString[cmdMAX_INPUT_SIZE];
	portBASE_TYPE xReturned;

	(void) pvParameters;

	/* This semaphore is used to allow the task to wait for the Tx to complete
	without wasting any CPU time. */
	vSemaphoreCreateBinary( xTxCompleteSemaphore );
	configASSERT( xTxCompleteSemaphore );

	/* This semaphore is used to allow the task to block for an Rx to complete
	without wasting any CPU time. */
	vSemaphoreCreateBinary( xRxCompleteSemaphore );
	configASSERT( xRxCompleteSemaphore );

	/* Take the semaphores so they start in the wanted state.  A block time is
	not necessary, and is therefore set to 0, as it is known that the semaphores
	exists - they have just been created. */
	xSemaphoreTake( xTxCompleteSemaphore, 0 );
	xSemaphoreTake( xRxCompleteSemaphore, 0 );

	/* Obtain the address of the output buffer.  Note there is no mutual
	 exclusion on this buffer as it is assumed only one command console
	 interface will be used at any one time. */
	pcOutputString = FreeRTOS_CLIGetOutputBuffer();

	/* Send the welcome message. */
	prvSendBuffer(pcWelcomeMessage, strlen(pcWelcomeMessage));

	for (;;) {
		/* Wait for the next character to arrive.  A semaphore is used to
		 ensure no CPU time is used until data has arrived. */
		// Receive interrupt and give semaphore. //usart_read_buffer_job(&xCDCUsart, (uint8_t*) &cRxedChar, sizeof(cRxedChar));
		HAL_UART_Receive_IT(&huart3, (uint8_t*)&cRxedChar, sizeof(cRxedChar));
		if (xSemaphoreTake(xRxCompleteSemaphore, portMAX_DELAY) == pdPASS) {
			/* Echo the character back. */
			prvSendBuffer(&cRxedChar, sizeof(cRxedChar));

			/* Was it the end of the line? */
			if (cRxedChar == '\n' || cRxedChar == '\r') {
				/* Just to space the output from the input. */
				prvSendBuffer(pcNewLine, strlen(pcNewLine));

				/* See if the command is empty, indicating that the last command is
				 to be executed again. */
				if (ucInputIndex == 0) {
					/* Copy the last command back into the input string. */
					strcpy(cInputString, cLastInputString);
				}

				/* Pass the received command to the command interpreter.  The
				 command interpreter is called repeatedly until it returns pdFALSE
				 (indicating there is no more output) as it might generate more than
				 one string. */
				do {
					/* Get the next output string from the command interpreter. */
					xReturned = FreeRTOS_CLIProcessCommand(cInputString,
							pcOutputString, configCOMMAND_INT_MAX_OUTPUT_SIZE);

					/* Write the generated string to the UART. */
					prvSendBuffer(pcOutputString, strlen(pcOutputString));

				} while (xReturned != pdFALSE);

				/* All the strings generated by the input command have been sent.
				 Clear the input	string ready to receive the next command.  Remember
				 the command that was just processed first in case it is to be
				 processed again. */
				strcpy(cLastInputString, cInputString);
				ucInputIndex = 0;
				memset(cInputString, 0x00, cmdMAX_INPUT_SIZE);

				prvSendBuffer(pcEndOfOutputMessage, strlen(pcEndOfOutputMessage));
			} else {
				if (cRxedChar == '\r') {
					/* Ignore the character. */
				} else if ((cRxedChar == '\b') || (cRxedChar == cmdASCII_DEL)) {
					/* Backspace was pressed.  Erase the last character in the
					 string - if any. */
					if (ucInputIndex > 0) {
						ucInputIndex--;
						cInputString[ucInputIndex] = '\0';
					}
				} else {
					/* A character was entered.  Add it to the string
					 entered so far.  When a \n is entered the complete
					 string will be passed to the command interpreter. */
					if ((cRxedChar >= ' ') && (cRxedChar <= '~')) {
						if (ucInputIndex < cmdMAX_INPUT_SIZE) {
							cInputString[ucInputIndex] = cRxedChar;
							ucInputIndex++;
						}
					}
				}
			}
		}
	}
}
