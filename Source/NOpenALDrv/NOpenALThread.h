/*=============================================================================
	NOpenALThread.h: pthreads shim for NOpenALDrv, API-compatible with the UE1
	fork's UnThread.h (UT99's Core has no threading API).
=============================================================================*/
#pragma once

// Core's clock() macro (UnFile.h) mangles <time.h> - suspend it for <pthread.h>.
#pragma push_macro("clock")
#undef clock
#include <pthread.h>
#pragma pop_macro("clock")

typedef void* UTHREAD;
typedef void* THREAD_RET;
typedef THREAD_RET ( *THREAD_FUNC )( void* Arg );

// Always joinable (spawns are always paired with appThreadJoin), so bDetach is ignored.
struct FALThreadRec
{
	pthread_t   Thread;
	THREAD_FUNC Func;
	void*       Arg;
};

static void* NOpenAL_ThreadTrampoline( void* P )
{
	FALThreadRec* Rec = (FALThreadRec*)P;
	return Rec->Func( Rec->Arg );
}

inline UTHREAD appThreadSpawn( THREAD_FUNC Func, void* Arg, const char* /*Name*/, UBOOL /*bDetach*/, DWORD* OutThreadId )
{
	FALThreadRec* Rec = new FALThreadRec;
	Rec->Func = Func;
	Rec->Arg  = Arg;
	if( pthread_create( &Rec->Thread, NULL, NOpenAL_ThreadTrampoline, Rec ) != 0 )
	{
		delete Rec;
		return NULL;
	}
	if( OutThreadId )
		*OutThreadId = 0;
	return Rec;
}

inline THREAD_RET appThreadJoin( UTHREAD Thread )
{
	FALThreadRec* Rec = (FALThreadRec*)Thread;
	void* Ret = NULL;
	pthread_join( Rec->Thread, &Ret );
	delete Rec;
	return Ret;
}

// Recursive: NOpenALDrv locks re-entrantly (UnregisterMusic -> StopMusic).
class FMutex
{
public:
	FMutex( const char* /*InName*/ )
	{
		pthread_mutexattr_t Attr;
		pthread_mutexattr_init( &Attr );
		pthread_mutexattr_settype( &Attr, PTHREAD_MUTEX_RECURSIVE );
		pthread_mutex_init( &Handle, &Attr );
		pthread_mutexattr_destroy( &Attr );
	}
	~FMutex() { pthread_mutex_destroy( &Handle ); }
	void Lock()   { pthread_mutex_lock( &Handle ); }
	void Unlock() { pthread_mutex_unlock( &Handle ); }
private:
	pthread_mutex_t Handle;
};

class FScopedLock
{
public:
	FScopedLock( FMutex& InMutex ) : Mutex( InMutex ) { Mutex.Lock(); }
	~FScopedLock() { Mutex.Unlock(); }
private:
	FMutex& Mutex;
};
