export module shared:random;
import std;

export class Random
{
public:
	Random()
	{
		if (_initialized == false)
		{
			// Use non-deterministic generator to seed the faster Mersenne twister.
			auto seed = _randDevice();
			_mt.seed(seed);
			_initialized = true;
		}
	}

	int Uniform(int a, int b)
	{
		std::uniform_int_distribution<int> dist(a, b);
		return dist(_mt);
	}

	float Uniform(float a, float b)
	{
		std::uniform_real_distribution<float> dist(a, b);
		return dist(_mt);
	}

	double Uniform(double a, double b)
	{
		std::uniform_real_distribution<double> dist(a, b);
		return dist(_mt);
	}

private:
	static inline bool _initialized = false;
	static inline std::random_device _randDevice{};
	static inline std::mt19937 _mt{};
};
