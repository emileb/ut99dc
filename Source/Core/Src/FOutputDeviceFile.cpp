/*=============================================================================
	FOutputDeviceFile.cpp: ANSI file output device implementation.
	Copyright 1997-1999 Epic Games, Inc. All Rights Reserved.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "CorePrivate.h"
#include "FOutputDeviceFile.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO,"UT99", __VA_ARGS__))
// From Clibs_OpenTouch (linked into the final .so); prototype declared here so
// this Core module needs no extra include paths. Mirrors every log line to
// logcat and to the app's shareable log file.
extern "C" void LogWritter_Write(const char *msg);
#endif

/*-----------------------------------------------------------------------------
	FOutputDeviceFile implementation.
-----------------------------------------------------------------------------*/

void FOutputDeviceFile::Serialize( const TCHAR* Data, enum EName Event )
{
	static UBOOL Entry=0;
	if( !GIsCriticalError || Entry )
	{
		if( !FName::SafeSuppressed(Event) )
		{
#ifdef __ANDROID__
			if( Event!=NAME_Title )
			{
				LOGI( "%s: %s", TCHAR_TO_ANSI(FName::SafeString(Event)), TCHAR_TO_ANSI(Data) );
				LogWritter_Write( TCHAR_TO_ANSI(Data) );
			}
#endif
			if( !LogAr && !Dead )
			{
				// Make log filename.
				if( !Filename[0] )
				{
					appStrcpy( Filename, appBaseDir() );
					if
					(	!Parse(appCmdLine(), TEXT("LOG="), Filename+appStrlen(Filename), ARRAY_COUNT(Filename)-appStrlen(Filename) )
					&&	!Parse(appCmdLine(), TEXT("ABSLOG="), Filename, ARRAY_COUNT(Filename) ) )
					{
						appStrcat( Filename, appPackage() );
						appStrcat( Filename, TEXT(".log") );
					}
				}

				// Open log file.
				LogAr = GFileManager->CreateFileWriter( Filename, FILEWRITE_AllowRead|FILEWRITE_Unbuffered|(Opened?FILEWRITE_Append:0));
				if( LogAr )
				{
					Opened = 1;
#if UNICODE && !FORCE_ANSI_LOG
					_WORD UnicodeBOM = UNICODE_BOM;
					LogAr->Serialize( &UnicodeBOM, 2 );
#endif
					Logf( NAME_Log, TEXT("Log file open, %s"), appTimestamp() );
				}
				else Dead = 1;
			}
			if( LogAr && Event!=NAME_Title )
			{
#if FORCE_ANSI_LOG && UNICODE
				TCHAR Ch[1024];
				ANSICHAR ACh[1024];
				appSprintf( Ch, TEXT("%s: %s%s"), FName::SafeString(Event), Data, LINE_TERMINATOR );
				for( INT i=0; Ch[i]; i++ )
					ACh[i] = ToAnsi(Ch[i] );
				ACh[i] = 0;
				LogAr->Serialize( ACh, i );
#else
				WriteRaw( FName::SafeString(Event) );
				WriteRaw( TEXT(": ") );
				WriteRaw( Data );
				WriteRaw( LINE_TERMINATOR );
#endif
			}
			if( GLogHook )
				GLogHook->Serialize( Data, Event );
		}
	}
	else
	{
		Entry=1;
		try
		{
			// Ignore errors to prevent infinite-recursive exception reporting.
			Serialize( Data, Event );
		}
		catch( ... )
		{}
		Entry=0;
	}
}

void FOutputDeviceFile::WriteRaw( const TCHAR* C )
{
	LogAr->Serialize( const_cast<TCHAR*>(C), appStrlen(C)*sizeof(TCHAR) );
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
