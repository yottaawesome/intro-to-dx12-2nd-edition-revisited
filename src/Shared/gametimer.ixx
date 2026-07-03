//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:gametimer;
import std;
import :win32;

export class GameTimer
{
public:
	GameTimer()
	{
		auto countsPerSec = std::int64_t{};
		Win32::QueryPerformanceFrequency((Win32::LARGE_INTEGER*)&countsPerSec);
		mSecondsPerCount = 1.0 / (double)countsPerSec;
	}

	// Returns the total time elapsed since Reset() was called, NOT counting any
	// time when the clock is stopped.
	auto TotalTime() const -> float
	{
		// If we are stopped, do not count the time that has passed since we stopped.
		// Moreover, if we previously already had a pause, the distance 
		// mStopTime - mBaseTime includes paused time, which we do not want to count.
		// To correct this, we can subtract the paused time from mStopTime:  
		//
		//                     |<--paused time-->|
		// ----*---------------*-----------------*------------*------------*------> time
		//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

		if (mStopped)
		{
			return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
		}
		// The distance mCurrTime - mBaseTime includes paused time,
		// which we do not want to count.  To correct this, we can subtract 
		// the paused time from mCurrTime:  
		//
		//  (mCurrTime - mPausedTime) - mBaseTime 
		//
		//                     |<--paused time-->|
		// ----*---------------*-----------------*------------*------> time
		//  mBaseTime       mStopTime        startTime     mCurrTime
		else
		{
			return (float)(((mCurrTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
		}
	}

	auto DeltaTime() const -> float
	{
		return (float)mDeltaTime;
	}

	void Reset()
	{
		auto currTime = std::int64_t{};
		Win32::QueryPerformanceCounter((Win32::LARGE_INTEGER*)&currTime);

		mBaseTime = currTime;
		mPrevTime = currTime;
		mStopTime = 0;
		mStopped = false;
	}

	void Start()
	{
		if (not mStopped)
			return;

		// Accumulate the time elapsed between stop and start pairs.
		//
		//                     |<-------d------->|
		// ----*---------------*-----------------*------------> time
		//  mBaseTime       mStopTime        startTime     
		auto startTime = std::int64_t{};
		Win32::QueryPerformanceCounter((Win32::LARGE_INTEGER*)&startTime);
		mPausedTime += (startTime - mStopTime);
		mPrevTime = startTime;
		mStopTime = 0;
		mStopped = false;
	}

	void Stop()
	{
		if (mStopped)
			return;
		auto currTime = std::int64_t{};
		Win32::QueryPerformanceCounter((Win32::LARGE_INTEGER*)&currTime);
		mStopTime = currTime;
		mStopped = true;
	}

	void Tick()
	{
		if (mStopped)
		{
			mDeltaTime = 0.0;
			return;
		}

		auto currTime = std::int64_t{};
		Win32::QueryPerformanceCounter((Win32::LARGE_INTEGER*)&currTime);
		mCurrTime = currTime;

		// Time difference between this frame and the previous.
		mDeltaTime = (mCurrTime - mPrevTime) * mSecondsPerCount;

		// Prepare for next frame.
		mPrevTime = mCurrTime;

		// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
		// processor goes into a power save mode or we get shuffled to another
		// processor, then mDeltaTime can be negative.
		if (mDeltaTime < 0.0)
			mDeltaTime = 0.0;
	}

private:
	double mSecondsPerCount = 0;
	double mDeltaTime = -1.0;

	std::int64_t mBaseTime = 0;
	std::int64_t mPausedTime = 0;
	std::int64_t mStopTime = 0;
	std::int64_t mPrevTime = 0;
	std::int64_t mCurrTime = 0;

	bool mStopped = false;
};
