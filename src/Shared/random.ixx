//***************************************************************************************
// d3dApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

export module shared:random;
import std;

export class Random
{
public:
	auto Uniform(int a, int b) -> int
	{
		auto dist = std::uniform_int_distribution<int>{ a, b };
		return dist(_mt);
	}

	auto Uniform(float a, float b) -> float
	{
		auto dist = std::uniform_real_distribution<float>{ a, b };
		return dist(_mt);
	}

	auto Uniform(double a, double b) -> double
	{
		auto dist = std::uniform_real_distribution<double>{ a, b };
		return dist(_mt);
	}

private:
	static inline auto _mt = 
		[] -> std::mt19937 
		{ 
			auto randDevice = std::random_device{};
			auto mt = std::mt19937{};
			// Use non-deterministic generator to seed the faster Mersenne twister.
			mt.seed(randDevice()); 
			return mt; 
		}();
};
