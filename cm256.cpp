/*
    CM256 Library
    based on GF256 Library

    No software license, use at your own risk.
*/

#include "cm256.h"


/*
    GF(256) Cauchy Matrix Overview

    As described on Wikipedia, each element of a normal Cauchy matrix is defined as:

        a_ij = 1 / (x_i - y_j)
        The arrays x_i and y_j are vector parameters of the matrix.
        The values in x_i cannot be reused in y_j.

    Moving beyond the Wikipedia...

    (1) Number of rows (R) is the range of i, and number of columns (C) is the range of j.

    (2) Being able to select x_i and y_j makes Cauchy matrices more flexible in practice
        than Vandermonde matrices, which only have one parameter per row.

    (3) Cauchy matrices are always invertible, AKA always full rank, AKA when treated as
        as linear system y = M*x, the linear system has a single solution.

    (4) A Cauchy matrix concatenated below a square CxC identity matrix always has rank C,
        Meaning that any R rows can be eliminated from the concatenated matrix and the
        matrix will still be invertible.  This is how Reed-Solomon erasure codes work.

    (5) Any row or column can be multiplied by a constant, and the resulting matrix is
        still full rank.

    (6) Matrix elements with a value of 1 are much faster to operate on than other values.
        For instance a matrix of [1, 1, 1, 1, 1] is invertible and much faster for various
        purposes than [2, 2, 2, 2, 2].

    (7) For GF(256) matrices, the symbols in x_i and y_j are selected from the numbers
        0...255, and so the number of rows + number of columns may not exceed 256.
        Note that values in x_i and y_j may not be reused as stated above.

    In summary, Cauchy matrices
        are preferred over Vandermonde matrices.  (2)
        are great for MDS erasure codes.  (3) and (4)
        should be optimized to include more 1 elements.  (5) and (6)
        have a limited size in GF(256), rows+cols <= 256.  (7)
*/


// Context object for GF(256) math
static GF256_ALIGNED gf256_ctx GF256Ctx;


//-----------------------------------------------------------------------------
// Initialization

extern "C" int cm256_init_(int version)
{
    if (version != CM256_VERSION)
    {
        // User's header does not match library version
        return -10;
    }

    // Return error code from GF(256) init if required
    return gf256_init(&GF256Ctx);
}


/*
    Selected Cauchy Matrix Form

    The matrix consists of elements a_ij, where i = row, j = column.
    a_ij = 1 / (x_i - y_j), where x_i and y_j are sets of GF(256) values
    that do not intersect.

    We select x_i and y_j to just be incrementing numbers for the
    purposes of this library.  Further optimizations may yield matrices
    with more 1 elements, but the benefit seems relatively small.

    The x_i values range from 0...(originalCount - 1).
    The y_j values range from originalCount...(originalCount + recoveryCount - 1).

    We then improve the Cauchy matrix by dividing each column by the
    first row element of that column.  The result is an invertible
    matrix that has all 1 elements in the first row.  This is equivalent
    to a rotated Vandermonde matrix, so we could have used one of those.

    The advantage of doing this is that operations involving the first
    row will be extremely fast (just memory XOR), so the decoder can
    be optimized to take advantage of the shortcut when the first
    recovery row can be used.

    First row element of Cauchy matrix for each column:
    a_0j = 1 / (x_0 - y_j) = 1 / (x_0 - y_j)

    Our Cauchy matrix sets first row to ones, so:
    a_ij = (1 / (x_i - y_j)) / a_0j
    a_ij = (y_j - x_0) / (x_i - y_j)
    a_ij = (y_j + x_0) div (x_i + y_j) in GF(256)
*/

// This function generates each matrix element based on x_i, x_0, y_j
// Note that for x_i == x_0, this will return 1, so it is better to unroll out the first row.
static GF256_FORCE_INLINE unsigned char GetMatrixElement(unsigned char x_i, unsigned char x_0, unsigned char y_j)
{
    return gf256_div(&GF256Ctx, gf256_add(y_j, x_0), gf256_add(x_i, y_j));
}


//-----------------------------------------------------------------------------
// Encoding

extern "C" int cm256_encode(
    cm256_encoder_params params, // Encoder params
    unsigned char** originals,   // Array of pointers to original blocks
    void* recoveryBlocks)        // Output recovery blocks end-to-end
{
    // Validate input:
    if (params.OriginalCount <= 0 ||
        params.RecoveryCount <= 0 ||
        params.BlockBytes <= 0)
    {
        return -1;
    }
    if (params.OriginalCount + params.RecoveryCount > 256)
    {
        return -2;
    }
    if (!originals || !recoveryBlocks)
    {
        return -3;
    }

    uint8_t* recoveryBlock = static_cast<uint8_t*>(recoveryBlocks);

    // If only one block of input data,
    if (params.OriginalCount == 1)
    {
        // No meaningful operation here, degenerate to outputting the same data each time.

        for (int i = 0; i < params.RecoveryCount; ++i)
        {
            memcpy(recoveryBlock + i * params.BlockBytes, originals[0], params.BlockBytes);
        }

        return 0;
    }
    // else OriginalCount >= 2:

    // Unroll first row of recovery matrix:
    // The matrix we generate for the first row is all ones,
    // so it is merely a parity of the original data.
    gf256_addset_mem(recoveryBlock, originals[0], originals[1], params.BlockBytes);
    for (int j = 2; j < params.OriginalCount; ++j)
    {
        gf256_add_mem(recoveryBlock, originals[j], params.BlockBytes);
    }

    // Start the x_0 values arbitrarily from the original count.
    const uint8_t x_0 = static_cast<uint8_t>(params.OriginalCount);

    // For each remaining recovery block row,
    for (int i = 1; i < params.RecoveryCount; ++i)
    {
        const uint8_t x_i = static_cast<uint8_t>(x_0 + i);

        recoveryBlock += params.BlockBytes;

        // Unroll first operation for speed
        {
            const uint8_t y_0 = 0;
            const uint8_t matrixElement = GetMatrixElement(x_i, x_0, y_0);

            gf256_mul_mem(&GF256Ctx, recoveryBlock, originals[0], matrixElement, params.BlockBytes);
        }

        // For each original data column,
        for (int j = 1; j < params.OriginalCount; ++j)
        {
            const uint8_t y_j = static_cast<uint8_t>(j);
            const uint8_t matrixElement = GetMatrixElement(x_i, x_0, y_j);

            gf256_muladd_mem(&GF256Ctx, recoveryBlock, matrixElement, originals[j], params.BlockBytes);
        }
    }

    return 0;
}


//-----------------------------------------------------------------------------
// Decoding

struct Decoder
{
    // Encode parameters
    cm256_encoder_params Params;

    // Recovery blocks
    cm256_block* Recovery[256];
    int RecoveryCount;

    // Original blocks
    cm256_block* Original[256];
    int OriginalCount;

    // Row indices that were erased
    uint8_t ErasuresIndices[256];

    // Initialize the decoder
    void Initialize(cm256_encoder_params& params, cm256_block* blocks);

    // Decode m=1 case
    void DecodeM1();

    // Decode for m>1 case
    void Decode();
};

void Decoder::Initialize(cm256_encoder_params& params, cm256_block* blocks)
{
    Params = params;

    cm256_block* block = blocks;
    OriginalCount = 0;
    RecoveryCount = 0;

    // Initialize erasures to zeros
    for (int ii = 0; ii < params.OriginalCount; ++ii)
    {
        ErasuresIndices[ii] = 0;
    }

    // For each input block,
    for (int ii = 0; ii < params.OriginalCount; ++ii, ++block)
    {
        int row = block->Index;

        // If it is an original block,
        if (row < params.OriginalCount)
        {
            Original[OriginalCount++] = block;
            ErasuresIndices[row] = 1;
        }
        else
        {
            Recovery[RecoveryCount++] = block;
        }
    }

    // Identify erasures
    for (int ii = 0, indexCount = 0; ii < 256; ++ii)
    {
        if (!ErasuresIndices[ii])
        {
            ErasuresIndices[indexCount] = static_cast<uint8_t>( ii );

            if (++indexCount >= RecoveryCount)
            {
                break;
            }
        }
    }
}

void Decoder::DecodeM1()
{
    // XOR all other blocks into the recovery block
    uint8_t* outBlock = Recovery[0]->Block;
    const uint8_t* inBlock = nullptr;

    // For each block,
    for (int ii = 0; ii < OriginalCount; ++ii)
    {
        const uint8_t* inBlock2 = Original[ii]->Block;

        if (!inBlock)
        {
            inBlock = inBlock2;
        }
        else
        {
            // outBlock ^= inBlock ^ inBlock2
            gf256_add2_mem(outBlock, inBlock, inBlock2, Params.BlockBytes);
            inBlock = nullptr;
        }
    }

    // Complete XORs
    if (inBlock)
    {
        gf256_add_mem(outBlock, inBlock, Params.BlockBytes);
    }

    // Recover the index it corresponds to
    Recovery[0]->Index = ErasuresIndices[0];
}

void Decoder::Decode()
{
    // Start the x_0 values arbitrarily from the original count.
    const uint8_t x_0 = static_cast<uint8_t>(Params.OriginalCount);

    // Eliminate original data from the the recovery rows
    for (int originalIndex = 0; originalIndex < OriginalCount; ++originalIndex)
    {
        const uint8_t* inBlock = Original[originalIndex]->Block;
        const uint8_t inRow = Original[originalIndex]->Index;

        for (int recoveryIndex = 0; recoveryIndex < RecoveryCount; ++recoveryIndex)
        {
            uint8_t* outBlock = Recovery[recoveryIndex]->Block;
            const uint8_t x_i = Recovery[recoveryIndex]->Index;
            const uint8_t y_j = inRow;
            const uint8_t matrixElement = GetMatrixElement(x_i, x_0, y_j);

            gf256_muladd_mem(&GF256Ctx, outBlock, matrixElement, inBlock, Params.BlockBytes);
        }
    }

    // Allocate matrix
    static const int StackAllocSize = 2048;
    uint8_t stackMatrix[StackAllocSize];
    uint8_t* dynamicMatrix = nullptr;
    uint8_t* matrix = stackMatrix;
    if (RecoveryCount * RecoveryCount > StackAllocSize)
    {
        dynamicMatrix = new uint8_t[RecoveryCount * RecoveryCount];
        matrix = dynamicMatrix;
    }

    // Fill matrix
    uint8_t* elementPtr = matrix;
    for (int i = 0; i < RecoveryCount; ++i)
    {
        const uint8_t x_i = Recovery[i]->Index;

        for (int j = 0; j < RecoveryCount; ++j)
        {
            const uint8_t y_j = ErasuresIndices[j];
            const uint8_t matrixElement = GetMatrixElement(x_i, x_0, y_j);

            *elementPtr++ = matrixElement;
        }
    }

    // Initialize pivots to 0...255
    uint8_t pivots[256];
    for (int j = 0; j < Params.OriginalCount; ++j)
    {
        pivots[j] = static_cast<uint8_t>(j);
    }

    // Gaussian elimination
    // Puts matrix into upper-triangular form
    for (int j = 0; j < RecoveryCount; ++j)
    {
        // Hunt for pivot in column (guaranteed to find one)
        for (int remainingIndex = j; remainingIndex < RecoveryCount; ++remainingIndex)
        {
            // Look up recovery row from pivots array
            unsigned i = pivots[remainingIndex];

            // TBD: Find some way to avoid multiplication here?
            uint8_t* elementPtr = matrix + i * RecoveryCount + j;
            uint8_t matrixElement = *elementPtr;
            if (matrixElement == 0)
            {
                continue;
            }
            ++elementPtr;

            // Swap pivots
            pivots[remainingIndex] = pivots[j];
            pivots[j] = static_cast<uint8_t>(i);

            // Set the index
            Recovery[i]->Index = ErasuresIndices[j];

            // Get the block pointer
            uint8_t* block = Recovery[i]->Block;

            if (matrixElement != 1)
            {
                // Divide remainder of row and its block by element
                const uint8_t invMatrixElement = gf256_inv(&GF256Ctx, matrixElement);
                gf256_mul_mem(&GF256Ctx, elementPtr, elementPtr, invMatrixElement, RecoveryCount - j - 1);
                gf256_mul_mem(&GF256Ctx, block, block, invMatrixElement, Params.BlockBytes);
            }

            // Remove it from all the other data
            for (int k = j + 1; k < RecoveryCount; ++k)
            {
                // Look up row element for next remaining row
                unsigned otheri = pivots[k];
                uint8_t* otherElementPtr = matrix + otheri * RecoveryCount + j;
                uint8_t otherMatrixElement = *otherElementPtr;

                // Eliminate the element
                gf256_muladd_mem(&GF256Ctx, otherElementPtr + 1, otherMatrixElement, elementPtr, RecoveryCount - j - 1);
                gf256_muladd_mem(&GF256Ctx, Recovery[otheri]->Block, otherMatrixElement, block, Params.BlockBytes);
            }

            break;
        }
    }

    // Back-substitute
    // Diagonalizes the matrix
    for (int j = RecoveryCount - 2; j >= 0; --j)
    {
        // Look up recovery row from pivots array
        // Get the block pointer
        const uint8_t jIndex = pivots[j];
        uint8_t* block = Recovery[jIndex]->Block;
        uint8_t* row = matrix + jIndex * RecoveryCount;

        // For each uncleared row element,
        for (int k = RecoveryCount - 1; k > j; --k)
        {
            // Grab matrix element
            const uint8_t kIndex = pivots[k];
            uint8_t matrixElement = row[k];

            // Eliminate the element
            gf256_muladd_mem(&GF256Ctx, block, matrixElement, Recovery[kIndex]->Block, Params.BlockBytes);
        }
    }

    delete[] dynamicMatrix;
}

extern "C" int cm256_decode(
    cm256_encoder_params params, // Encoder params
    cm256_block* blocks)         // Array of 'originalCount' blocks as described above
{
    if (params.OriginalCount <= 0 ||
        params.RecoveryCount <= 0 ||
        params.BlockBytes <= 0)
    {
        return -1;
    }
    if (params.OriginalCount + params.RecoveryCount > 256)
    {
        return -2;
    }

    // If there is only one block,
    if (params.OriginalCount == 1)
    {
        // It is the same block repeated
        blocks[0].Index = 0;
        return 0;
    }

    Decoder state;
    state.Initialize(params, blocks);

    // If nothing is erased,
    if (state.RecoveryCount <= 0)
    {
        return 0;
    }

    // If m=1,
    if (params.RecoveryCount == 1)
    {
        state.DecodeM1();
        return 0;
    }

    // Decode for m>1
    state.Decode();
    return 0;
}