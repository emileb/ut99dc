/*=============================================================================
	NOpenALThread.h: self-contained POSIX threading shim for NOpenALDrv.

	The sibling UE1 fork carries a Core threading API (UnThread.h:
	appThreadSpawn/FMutex/FScopedLock). UT99's Core has none, so the OpenAL
	driver's music-streaming thread uses these local pthreads wrappers instead -
	API-compatible with UE1's UnThread.h so NOpenALDrv.cpp compiles unchanged.
	POSIX-only, which covers both Android and the macOS desktop build.
=============================================================================*/
#pragma once

// This header is included after Core, which defines a function-like clock()
// timing macro (UnFile.h) that mangles <time.h>'s `clock_t clock(void)`
// declaration pulled in transitively by <pthread.h>. Suspend it across the
// include.
#pragma push_macro("clock")
#undef clock
#include <pthread.h>
#pragma pop_macro("clock")

typedef void* UTHREAD;
typedef void* THREAD_RET;
typedef THREAD_RET ( *THREAD_FUNC )( void* Arg );

// Internal thread record. Always joinable: NOpenALDrv always pairs a spawn with
// appThreadJoin() in StopMusicThread(), so we ignore bDetach (detaching would
// make the later join undefined).
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

// Recursive mutex - NOpenALDrv locks re-entrantly (e.g. UnregisterMusic holds
// the lock and calls StopMusic, which locks again).
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
