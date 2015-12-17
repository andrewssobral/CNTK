// TensorView.cpp -- main CPP file that contains all functions exported by the CNTKMath.dll
//
// <copyright file="Matrix.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//

// This implements the TensorView class, which is a layer around Matrix that reinterprets its content as a generic tensor.

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "stdafx.h"
#include "Basics.h"
#include "TensorView.h"
#include <array>

#ifndef let
#define let const auto
#endif

namespace Microsoft { namespace MSR { namespace CNTK {

    using namespace std;

    // -------------------------------------------------------------------
    // construction
    // -------------------------------------------------------------------

    // cast a matrix as a TensorView
    template<class ElemType>
    TensorView<ElemType>::TensorView(Matrix<ElemType> & sob) :
        m_sob(&sob), m_shape(TensorShape(array<size_t, 2> { sob.GetNumRows(), sob.GetNumCols() }))
    { }
    // reshape a TensorView
    template<class ElemType>
    TensorView<ElemType>::TensorView(const TensorView<ElemType> & other, const TensorShape & shape) :
        m_sob(other.m_sob), m_shape(shape)
    {
        // for now we enforce that tensor dimensions match dimensions of the underlying matrix storage object
        // This is for sanity checks. In the future, it may appropriate to reduce this check to just checking the total number of elements, to allow abuses.
        // TODO: Use the multipliers instead?
        size_t i;
        size_t rowDim = 1;
        for (i = 0; i < m_shape.size() && rowDim < m_sob->GetNumRows(); i++)
            rowDim *= m_shape[i];
        // first i dimensions match matrix row dimension
        size_t colDim = 1;
        for (; i < m_shape.size(); i++)
            colDim *= m_shape[i];
        if (rowDim != m_sob->GetNumRows() || colDim != m_sob->GetNumCols())
            LogicError("TensorView: Tensor dimensions %s do not match storage-object dims %d x %d", string(m_shape).c_str(), (int)m_sob->GetNumRows(), (int)m_sob->GetNumCols());
    }

    // -------------------------------------------------------------------
    // elementwise operations
    // -------------------------------------------------------------------

    static bool Matches(size_t d1, size_t d2) { return d1 == 1 || d2 == 1 || d1 == d2; }    // do two dimensions match?

    template<class ElemType>
    void TensorView<ElemType>::DoBinaryOpOf(ElemType beta, const TensorView & a, const TensorView & b, ElemType alpha, ElementWiseOperator op)
    {
#define N 3     // later make this a template parameter. N=1 is possible for generators, such as constants.
        array<TensorShape, N> shapes;
        TensorView & c = *this;

        shapes[0] = a.GetShape();
        shapes[1] = b.GetShape();
        shapes[2] = c.GetShape();       // last one is the output

        // massage TensorShapes
        // Note that TensorShapes here may be shapes are stored or shapes with stride magic applied.

        // expand ones to make tensors compatible
        // Trailing dimensions broadcast.
        // E.g. A(J) vs. B(J x T) will broadcast A(:) to all T columns.
        // To broadcast an A(T) to all J rows of B, use TensorShape editing to insert a dimension to get A(1,T).
        size_t dims = 0;
        for (size_t i = 0; i < N; i++)
            if (dims < shapes[i].GetNumDims())
                dims = shapes[i].GetNumDims();
        for (size_t i = 0; i < N; i++)
            shapes[i] = shapes[i].Pad(dims);

        // determine operation shape (max over all dimensions)
        vector<size_t> opDims(dims, 0);
        for (size_t k = 0; k < dims; k++)
            for (size_t i = 0; i < N; i++)
                opDims[k] = max(opDims[k], shapes[i][k]);

        // dimension compatibility check
        // Each participant can broadcast. Non-broadcasting dimensions must match the operation dimension.
        for (size_t k = 0; k < dims; k++)
            for (size_t i = 0; i < N; i++)
                if (!Matches(shapes[i][k], opDims[k]))
                    InvalidArgument("Binary tensor operation: Dimension %d is incompatible between input %d and output (%s vs. %s)", (int)k, (int)shapes[i][k], string(shapes[i]).c_str(), string(TensorShape(opDims)).c_str());

        // flatten consecutive dimensions
        // Dimensions must be consecutive in memory, and either non-broadcasting or all-broadcasting, across all dimensions.
        // After this, as, bs, and cs no longer match the TensorShape objects.
        fprintf(stderr, "Pre-flatten: Op %d: %s op %s -> %s via %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());
        for (size_t k = 1; k < dims; k++)
        {
            for (size_t i = 0; i < N; i++)
            {
                // check if stored without gaps to skip
                if (!shapes[i].CanFlatten(k))
                    goto nope;
                // check if they are either all broadcasting or all not broadcasting
                if ((shapes[i][k] != opDims[k] || shapes[i][k - 1] != opDims[k - 1]) && (shapes[i][k] != 1 || shapes[i][k - 1] != 1))
                    goto nope;
            }
            // these dimensions can be merged
            for (size_t i = 0; i < N; i++)
                shapes[i] = shapes[i].Flatten(k);               // TODO: overdoing the immutable thingy much?
            opDims = TensorShape(opDims).Flatten(k).GetDims();  // (ugh)
        nope:;
        }
        fprintf(stderr, "Post-flatten: Op %d: %s op %s -> %s via %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());

        // remove singleton dimensions
        vector<bool> toDrop(dims, false);
        for (size_t k = 0; k < dims; k++)
        {
            for (size_t i = 0; i < N; i++)
                if (shapes[i][k] != 1)
                    goto neither;
            toDrop[k] = true;           // found an all-singleton dimensions
        neither:;
        }
        for (size_t i = 0; i < N; i++)
            shapes[i] = shapes[i].DropSingletonDims(toDrop);
        opDims = TensorShape(opDims).DropSingletonDims(toDrop).GetDims();    // (ugh)
        // note: if op is a scalar, then we end up with 0 dimensions here, which is allowed
        fprintf(stderr, "Post-drop: Op %d: %s op %s -> %s via %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());

        // determine broadcasting; that is, set strides to 0 for 1-dimensions
        // To be more precise, we should only set actually broadcasting dimensions to 0.
        // But since dimensions that are 1 across all args are eliminated, any 1 must be some form of broadcasting.
        // TODO: Do we need to allow other strides at this point in time? If not, broadcasting becomes a bit vector.
        for (size_t i = 0; i < N; i++)
            shapes[i] = shapes[i].WithBroadcastStrides();

        // determine inverse broadcasting dimensions
        // Inverse broadcasting dims are actual for loops in the kernel, whereas broadcasting input dims are handled by the thread index.
        // For regular input dims:
        //  - determine number of steps (product over opDims[.])
        //  - launch that many kernels
        //  - pass in:
        //     - total number of steps
        //     - strides for all inputs (with stride magic), separated by regular and inverse broadcasting dimensions
        //     - opDim (no stride magic allowed) for regular broadcasting dimensions
        //     - reverse broadcasting dimensions
        //     - opcodes for elementwise op and reduction op
        //  - in each kernel:
        //     - map thread index to dimensions (regular broadcasting ones)
        //     - for-loop over inverse broadcasting dimensions
        //        - map dimensions (including inverse broadcasting) for every input
        //        - perform op on the input values
        //        - accumulate
        //     - map dimensions (regular) for output
        //     - save result

        // now perform the operation
        fprintf(stderr, "Op %d: %s  op  %s  ->  %s  via  %s\n", (int)op, string(shapes[0]).c_str(), string(shapes[1]).c_str(), string(shapes[2]).c_str(), string(TensorShape(opDims)).c_str());
        // :)
        beta; alpha;
    }

    // simple test function for testing stuff
    // Call as: Microsoft::MSR::CNTK::TensorView<float>::Test();
    template<class ElemType>
    /*static*/ void TensorView<ElemType>::Test()
    {
        Matrix<ElemType> m1(-1); m1.Resize(1, 42);
        Matrix<ElemType> m2(-1); m2.Resize(13, 1);
        Matrix<ElemType> m3(-1); m3.Resize(13, 21);
        TensorShape s1(1, 2, 21);
        TensorShape s2(13, 1);
        TensorShape s3(13, 1, 21);
        let t1 = TensorView<ElemType>(m1, s1); t1;
        let t2 = TensorView<ElemType>(m2, s2); t2;
        auto t3 = TensorView<ElemType>(m3, s3); t3;
        t3.DoSumOf(0, t1, t2, 1);
    }

    template class TensorView<float>;
    template class TensorView<double>;

}}}
