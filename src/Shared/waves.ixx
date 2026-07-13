export module shared:waves;
import std;
import :win32;

export class Waves
{
public:
    Waves(int m, int n, float dx, float dt, float speed, float damping)
    {
        mNumRows = m;
        mNumCols = n;

        mVertexCount = m * n;
        mTriangleCount = (m - 1) * (n - 1) * 2;

        mTimeStep = dt;
        mSpatialStep = dx;

        SetConstants(speed, damping);

        mPrevSolution.resize(m * n);
        mCurrSolution.resize(m * n);
        mNormals.resize(m * n);
        mTangentX.resize(m * n);

        // Generate grid vertices in system memory.

        auto halfWidth = (n - 1) * dx * 0.5f;
        auto halfDepth = (m - 1) * dx * 0.5f;
        for (auto i = 0; i < m; ++i)
        {
            auto z = halfDepth - i * dx;
            for (auto j = 0; j < n; ++j)
            {
                auto x = -halfWidth + j * dx;
                mPrevSolution[i * n + j] = DirectX::XMFLOAT3(x, 0.0f, z);
                mCurrSolution[i * n + j] = DirectX::XMFLOAT3(x, 0.0f, z);
                mNormals[i * n + j] = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
                mTangentX[i * n + j] = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
            }
        }
    }
    Waves(const Waves& rhs) = delete;
    Waves& operator=(const Waves& rhs) = delete;

    auto RowCount()const -> int
    {
        return mNumRows;
    }
    auto ColumnCount()const -> int
    {
        return mNumCols;
    }
    auto VertexCount()const -> int
    {
        return mVertexCount;
    }
    auto TriangleCount()const -> int
    {
        return mTriangleCount;
    }
    auto Width()const -> float
    {
        return mNumCols * mSpatialStep;
    }
    auto Depth()const -> float
    {
        return mNumRows * mSpatialStep;
    }

    // Returns the solution at the ith grid point.
    auto Position(int i)const -> const DirectX::XMFLOAT3& { return mCurrSolution[i]; }

    // Returns the solution normal at the ith grid point.
    auto Normal(int i)const -> const DirectX::XMFLOAT3& { return mNormals[i]; }

    // Returns the unit tangent vector at the ith grid point in the local x-axis direction.
    auto TangentX(int i)const -> const DirectX::XMFLOAT3& { return mTangentX[i]; }

    void SetConstants(float speed, float damping)
    {
        auto d = damping * mTimeStep + 2.0f;
        auto e = (speed * speed) * (mTimeStep * mTimeStep) / (mSpatialStep * mSpatialStep);
        mK1 = (damping * mTimeStep - 2.0f) / d;
        mK2 = (4.0f - 8.0f * e) / d;
        mK3 = (2.0f * e) / d;
    }

    void Update(float dt)
    {
        static auto t = 0.f;

        // Accumulate time.
        t += dt;

        // Only update the simulation at the specified time step.
        if (t < mTimeStep)
            return;
        // Only update interior points; we use zero boundary conditions.
        Concurrency::parallel_for(
            1,
            mNumRows - 1,
            [this](int i) //for(int i = 1; i < mNumRows-1; ++i)
            {
                for (auto j = 1; j < mNumCols - 1; ++j)
                {
                    // After this update we will be discarding the old previous
                    // buffer, so overwrite that buffer with the new update.
                    // Note how we can do this inplace (read/write to same element) 
                    // because we won't need prev_ij again and the assignment happens last.

                    // Note j indexes x and i indexes z: h(x_j, z_i, t_k)
                    // Moreover, our +z axis goes "down"; this is just to 
                    // keep consistent with our row indices going down.

                    mPrevSolution[i * mNumCols + j].y =
                        mK1 * mPrevSolution[i * mNumCols + j].y +
                        mK2 * mCurrSolution[i * mNumCols + j].y +
                        mK3 * (mCurrSolution[(i + 1) * mNumCols + j].y +
                            mCurrSolution[(i - 1) * mNumCols + j].y +
                            mCurrSolution[i * mNumCols + j + 1].y +
                            mCurrSolution[i * mNumCols + j - 1].y);
                }
            });

        // We just overwrote the previous buffer with the new data, so
        // this data needs to become the current solution and the old
        // current solution becomes the new previous solution.
        std::swap(mPrevSolution, mCurrSolution);

        t = 0.0f; // reset time

        //
        // Compute normals using finite difference scheme.
        Concurrency::parallel_for(
            1,
            mNumRows - 1,
            [this](int i) //for(int i = 1; i < mNumRows - 1; ++i)
            {
                for (int j = 1; j < mNumCols - 1; ++j)
                {
                    float l = mCurrSolution[i * mNumCols + j - 1].y;
                    float r = mCurrSolution[i * mNumCols + j + 1].y;
                    float t = mCurrSolution[(i - 1) * mNumCols + j].y;
                    float b = mCurrSolution[(i + 1) * mNumCols + j].y;
                    mNormals[i * mNumCols + j].x = -r + l;
                    mNormals[i * mNumCols + j].y = 2.0f * mSpatialStep;
                    mNormals[i * mNumCols + j].z = b - t;

                    auto n = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(XMLoadFloat3(&mNormals[i * mNumCols + j])) };
                    DirectX::XMStoreFloat3(&mNormals[i * mNumCols + j], n);

                    mTangentX[i * mNumCols + j] = DirectX::XMFLOAT3(2.0f * mSpatialStep, r - l, 0.0f);
                    auto T = DirectX::XMVECTOR{ DirectX::XMVector3Normalize(XMLoadFloat3(&mTangentX[i * mNumCols + j])) };
                    DirectX::XMStoreFloat3(&mTangentX[i * mNumCols + j], T);
                }
            });
    }
    void Disturb(int i, int j, float magnitude)
    {
        // Don't disturb boundaries.
        //assert(i > 1 && i < mNumRows - 2);
        //assert(j > 1 && j < mNumCols - 2);

        auto halfMag = 0.5f * magnitude;

        // Disturb the ijth vertex height and its neighbors.
        mCurrSolution[i * mNumCols + j].y += magnitude;
        mCurrSolution[i * mNumCols + j + 1].y += halfMag;
        mCurrSolution[i * mNumCols + j - 1].y += halfMag;
        mCurrSolution[(i + 1) * mNumCols + j].y += halfMag;
        mCurrSolution[(i - 1) * mNumCols + j].y += halfMag;
    }

private:
    int mNumRows = 0;
    int mNumCols = 0;

    int mVertexCount = 0;
    int mTriangleCount = 0;

    // Simulation constants we can precompute.
    float mK1 = 0.0f;
    float mK2 = 0.0f;
    float mK3 = 0.0f;

    float mTimeStep = 0.0f;
    float mSpatialStep = 0.0f;

    std::vector<DirectX::XMFLOAT3> mPrevSolution;
    std::vector<DirectX::XMFLOAT3> mCurrSolution;
    std::vector<DirectX::XMFLOAT3> mNormals;
    std::vector<DirectX::XMFLOAT3> mTangentX;
};
