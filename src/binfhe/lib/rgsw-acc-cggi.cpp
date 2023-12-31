//==================================================================================
// BSD 2-Clause License
//
// Copyright (c) 2014-2022, NJIT, Duality Technologies Inc. and other contributors
//
// All rights reserved.
//
// Author TPOC: contact@openfhe.org
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//==================================================================================

#include "rgsw-acc-cggi.h"

#include <string>
#include <iostream>
#include <iomanip>

using namespace std;

namespace lbcrypto {

// Key generation as described in Section 4 of https://eprint.iacr.org/2014/816
RingGSWACCKey RingGSWAccumulatorCGGI::KeyGenAcc(const std::shared_ptr<RingGSWCryptoParams> params,
                                                const NativePoly& skNTT, ConstLWEPrivateKey LWEsk) const {
    auto sv         = LWEsk->GetElement();
    int32_t mod     = sv.GetModulus().ConvertToInt();
    int32_t modHalf = mod >> 1;
    uint32_t n      = sv.GetLength();
    auto ek         = std::make_shared<RingGSWACCKeyImpl>(1, 2, n);

    // handles ternary secrets using signed mod 3 arithmetic; 0 -> {0,0}, 1 ->
    // {1,0}, -1 -> {0,1}
#pragma omp parallel for
    for (size_t i = 0; i < n; ++i) {
        int32_t s = (int32_t)sv[i].ConvertToInt();
        if (s > modHalf) {
            s -= mod;
        }

        switch (s) {
            case 0:
                (*ek)[0][0][i] = KeyGenCGGI(params, skNTT, 0);
                (*ek)[0][1][i] = KeyGenCGGI(params, skNTT, 0);
                break;
            case 1:
                (*ek)[0][0][i] = KeyGenCGGI(params, skNTT, 1);
                (*ek)[0][1][i] = KeyGenCGGI(params, skNTT, 0);
                break;
            case -1:
                (*ek)[0][0][i] = KeyGenCGGI(params, skNTT, 0);
                (*ek)[0][1][i] = KeyGenCGGI(params, skNTT, 1);
                break;
            default:
                std::string errMsg = "ERROR: only ternary secret key distributions are supported.";
                OPENFHE_THROW(not_implemented_error, errMsg);
        }
    }

    return ek;
}

void RingGSWAccumulatorCGGI::EvalAcc(const std::shared_ptr<RingGSWCryptoParams> params, const RingGSWACCKey ek,
                                     RLWECiphertext& acc, const NativeVector& a) const {
    auto mod        = a.GetModulus();
    uint32_t n      = a.GetLength();
    uint32_t M      = 2 * params->GetN();
    uint32_t modInt = mod.ConvertToInt();

    for (size_t i = 0; i < n; ++i) {
        // handles -a*E(1) and handles -a*E(-1) = a*E(1)
        AddToAccCGGI(params, (*ek)[0][0][i], (*ek)[0][1][i], mod.ModSub(a[i], mod) * (M / modInt), acc);
    }
}

// Encryption for the CGGI variant, as described in https://eprint.iacr.org/2020/086
RingGSWEvalKey RingGSWAccumulatorCGGI::KeyGenCGGI(const std::shared_ptr<RingGSWCryptoParams> params,
                                                  const NativePoly& skNTT, const LWEPlaintext& m) const {
    NativeInteger Q   = params->GetQ();
    uint32_t digitsG  = params->GetDigitsG();
    uint32_t digitsG2 = digitsG << 1;
    auto Gpow         = params->GetGPower();
    auto polyParams   = params->GetPolyParams();
    auto result       = std::make_shared<RingGSWEvalKeyImpl>(digitsG2, 2);

    DiscreteUniformGeneratorImpl<NativeVector> dug;
    dug.SetModulus(Q);

    // tempA is introduced to minimize the number of NTTs
    std::vector<NativePoly> tempA(digitsG2);

    for (size_t i = 0; i < digitsG2; ++i) {
        (*result)[i][0] = NativePoly(dug, polyParams, Format::COEFFICIENT);
        tempA[i]        = (*result)[i][0];
        (*result)[i][1] = NativePoly(params->GetDgg(), polyParams, Format::COEFFICIENT);
    }

    if (m > 0) {
        for (size_t i = 0; i < digitsG; ++i) {
            // Add G Multiple
            (*result)[2 * i][0][0].ModAddEq(Gpow[i], Q);
            // [a,as+e] + G
            (*result)[2 * i + 1][1][0].ModAddEq(Gpow[i], Q);
        }
    }

    // 3*digitsG2 NTTs are called
    result->SetFormat(Format::EVALUATION);
    for (size_t i = 0; i < digitsG2; ++i) {
        tempA[i].SetFormat(Format::EVALUATION);
        (*result)[i][1] += tempA[i] * skNTT;
    }

    return result;
}

// CGGI Accumulation as described in https://eprint.iacr.org/2020/086
// Added ternary MUX introduced in paper https://eprint.iacr.org/2022/074.pdf section 5
// We optimize the algorithm by multiplying the monomial after the external product
// This reduces the number of polynomial multiplications which further reduces the runtime
void RingGSWAccumulatorCGGI::AddToAccCGGI(const std::shared_ptr<RingGSWCryptoParams> params, const RingGSWEvalKey ek1,
                                          const RingGSWEvalKey ek2, const NativeInteger& a, RLWECiphertext& acc) const {
    // cycltomic order
    uint64_t MInt = 2 * params->GetN();
    NativeInteger M(MInt);
    uint32_t digitsG2 = params->GetDigitsG() << 1;
    auto polyParams   = params->GetPolyParams();

    std::vector<NativePoly> ct = acc->GetElements();
    std::vector<NativePoly> dct(digitsG2);

    // initialize dct to zeros
    for (size_t i = 0; i < digitsG2; ++i)
        dct[i] = NativePoly(polyParams, Format::COEFFICIENT, true);

    // calls 2 NTTs
    for (size_t i = 0; i < 2; ++i)
        ct[i].SetFormat(Format::COEFFICIENT);

    std::ofstream decomopose_a("/home/ove2/openfhe-development/Decomopose_A.txt"); 
    std::ofstream decomopose_b("/home/ove2/openfhe-development/Decomopose_B.txt"); 
    for (int i = 0; i < 1024; i++) {
        decomopose_a << ct[0].m_values.get()->m_data[i].m_value << "\n"; //m.m_data[dct_items].m_value
        decomopose_b << ct[1].m_values.get()->m_data[i].m_value << "\n"; 
    }
    decomopose_a.close();
    decomopose_b.close();

    SignedDigitDecompose(params, ct, dct);

    std::ofstream decomopose_a0("/home/ove2/openfhe-development/Decomopose_A0.txt"); 
    std::ofstream decomopose_a1("/home/ove2/openfhe-development/Decomopose_A1.txt"); 
    std::ofstream decomopose_a2("/home/ove2/openfhe-development/Decomopose_A2.txt"); 
    std::ofstream decomopose_a3("/home/ove2/openfhe-development/Decomopose_A3.txt"); 
    std::ofstream decomopose_b0("/home/ove2/openfhe-development/Decomopose_B0.txt"); 
    std::ofstream decomopose_b1("/home/ove2/openfhe-development/Decomopose_B1.txt"); 
    std::ofstream decomopose_b2("/home/ove2/openfhe-development/Decomopose_B2.txt"); 
    std::ofstream decomopose_b3("/home/ove2/openfhe-development/Decomopose_B3.txt"); 
    for (int i = 0; i < 1024; i++) {
        decomopose_a0 << dct[0].m_values.get()->m_data[i].m_value << "\n";
        decomopose_b0 << dct[1].m_values.get()->m_data[i].m_value << "\n"; 
        decomopose_a1 << dct[2].m_values.get()->m_data[i].m_value << "\n";
        decomopose_b1 << dct[3].m_values.get()->m_data[i].m_value << "\n";
        decomopose_a2 << dct[4].m_values.get()->m_data[i].m_value << "\n";
        decomopose_b2 << dct[5].m_values.get()->m_data[i].m_value << "\n"; 
        decomopose_a3 << dct[6].m_values.get()->m_data[i].m_value << "\n";
        decomopose_b3 << dct[7].m_values.get()->m_data[i].m_value << "\n";  
    }
    decomopose_a0.close();
    decomopose_b0.close();
    decomopose_a1.close();
    decomopose_b1.close();
    decomopose_a2.close();
    decomopose_b2.close();
    decomopose_a3.close();
    decomopose_b3.close();


    for (size_t i = 0; i < digitsG2; ++i)
        dct[i].SetFormat(Format::EVALUATION);

    // get data of dct input of externalProduct
    std::ofstream extPro_a0("/home/ove2/openfhe-development/ExtPro_A0.txt"); 
    std::ofstream extPro_a1("/home/ove2/openfhe-development/ExtPro_A1.txt"); 
    std::ofstream extPro_a2("/home/ove2/openfhe-development/ExtPro_A2.txt"); 
    std::ofstream extPro_a3("/home/ove2/openfhe-development/ExtPro_A3.txt"); 
    std::ofstream extPro_b0("/home/ove2/openfhe-development/ExtPro_B0.txt"); 
    std::ofstream extPro_b1("/home/ove2/openfhe-development/ExtPro_B1.txt"); 
    std::ofstream extPro_b2("/home/ove2/openfhe-development/ExtPro_B2.txt"); 
    std::ofstream extPro_b3("/home/ove2/openfhe-development/ExtPro_B3.txt"); 
    for (int i = 0; i < 1024; i++) {
        extPro_a0 << dct[0].m_values.get()->m_data[i].m_value << "\n";
        extPro_b0 << dct[1].m_values.get()->m_data[i].m_value << "\n"; 
        extPro_a1 << dct[2].m_values.get()->m_data[i].m_value << "\n";
        extPro_b1 << dct[3].m_values.get()->m_data[i].m_value << "\n";
        extPro_a2 << dct[4].m_values.get()->m_data[i].m_value << "\n";
        extPro_b2 << dct[5].m_values.get()->m_data[i].m_value << "\n"; 
        extPro_a3 << dct[6].m_values.get()->m_data[i].m_value << "\n";
        extPro_b3 << dct[7].m_values.get()->m_data[i].m_value << "\n";  
    }
    extPro_a0.close();
    extPro_b0.close();
    extPro_a1.close();
    extPro_b1.close();
    extPro_a2.close();
    extPro_b2.close();
    extPro_a3.close();
    extPro_b3.close();

    // First obtain both monomial(index) for sk = 1 and monomial(-index) for sk = -1
    auto aNeg         = M.ModSub(a, M);
    uint64_t indexPos = a.ConvertToInt();
    uint64_t indexNeg = aNeg.ConvertToInt();
    // index is in range [0,m] - so we need to adjust the edge case when
    // index = m to index = 0
    if (indexPos == MInt)
        indexPos = 0;
    if (indexNeg == MInt)
        indexNeg = 0;
    const NativePoly& monomial    = params->GetMonomial(indexPos);
    const NativePoly& monomialNeg = params->GetMonomial(indexNeg);

    // get the data of monomial and monomialNeg
    std::ofstream monomial_file("/home/ove2/openfhe-development/Monomial_INIT.txt"); 
    std::ofstream monomialNeg_file("/home/ove2/openfhe-development/MonomialNeg_INIT.txt"); 

    for (int i = 0; i < 1024; i += 8) {
        for (int j = 7; j >= 0; j--) {
            monomial_file    << setbase(16) << setw(7) << setfill('0') << monomial.m_values.get()->m_data[i+j].m_value;
            monomialNeg_file << setbase(16) << setw(7) << setfill('0') << monomialNeg.m_values.get()->m_data[i+j].m_value; 
        }
        monomial_file << "\n";
        monomialNeg_file << "\n";
    }
    monomial_file.close();
    monomialNeg_file.close();


    // acc = acc + dct * ek1 * monomial + dct * ek2 * negative_monomial;
    // uses in-place * operators for the last call to dct[i] to gain performance
    // improvement. Needs to be done using two loops for ternary secrets.
    // TODO (dsuponit): benchmark cases with operator*() and operator*=(). Make a copy of dct?
    const std::vector<std::vector<NativePoly>>& ev1 = ek1->GetElements();
    // get data of evk1 input of externalProduct
    std::ofstream extPro_evk1_a0("/home/ove2/openfhe-development/ExtPro_EVK1_A0.txt"); 
    std::ofstream extPro_evk1_a1("/home/ove2/openfhe-development/ExtPro_EVK1_A1.txt"); 
    std::ofstream extPro_evk1_a2("/home/ove2/openfhe-development/ExtPro_EVK1_A2.txt"); 
    std::ofstream extPro_evk1_a3("/home/ove2/openfhe-development/ExtPro_EVK1_A3.txt");
    std::ofstream extPro_evk1_a4("/home/ove2/openfhe-development/ExtPro_EVK1_A4.txt"); 
    std::ofstream extPro_evk1_a5("/home/ove2/openfhe-development/ExtPro_EVK1_A5.txt"); 
    std::ofstream extPro_evk1_a6("/home/ove2/openfhe-development/ExtPro_EVK1_A6.txt"); 
    std::ofstream extPro_evk1_a7("/home/ove2/openfhe-development/ExtPro_EVK1_A7.txt");  
    std::ofstream extPro_evk1_b0("/home/ove2/openfhe-development/ExtPro_EVK1_B0.txt"); 
    std::ofstream extPro_evk1_b1("/home/ove2/openfhe-development/ExtPro_EVK1_B1.txt"); 
    std::ofstream extPro_evk1_b2("/home/ove2/openfhe-development/ExtPro_EVK1_B2.txt"); 
    std::ofstream extPro_evk1_b3("/home/ove2/openfhe-development/ExtPro_EVK1_B3.txt"); 
    std::ofstream extPro_evk1_b4("/home/ove2/openfhe-development/ExtPro_EVK1_B4.txt"); 
    std::ofstream extPro_evk1_b5("/home/ove2/openfhe-development/ExtPro_EVK1_B5.txt"); 
    std::ofstream extPro_evk1_b6("/home/ove2/openfhe-development/ExtPro_EVK1_B6.txt"); 
    std::ofstream extPro_evk1_b7("/home/ove2/openfhe-development/ExtPro_EVK1_B7.txt"); 
    for (int i = 0; i < 1024; i += 8) {
        for (int j = 7; j >= 0; j--) {
            extPro_evk1_a0 << setbase(16) << setw(7) << setfill('0') << ev1[0][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_a1 << setbase(16) << setw(7) << setfill('0') << ev1[1][0].m_values.get()->m_data[i+j].m_value; 
            extPro_evk1_a2 << setbase(16) << setw(7) << setfill('0') << ev1[2][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_a3 << setbase(16) << setw(7) << setfill('0') << ev1[3][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_a4 << setbase(16) << setw(7) << setfill('0') << ev1[4][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_a5 << setbase(16) << setw(7) << setfill('0') << ev1[5][0].m_values.get()->m_data[i+j].m_value; 
            extPro_evk1_a6 << setbase(16) << setw(7) << setfill('0') << ev1[6][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_a7 << setbase(16) << setw(7) << setfill('0') << ev1[7][0].m_values.get()->m_data[i+j].m_value;  
            extPro_evk1_b0 << setbase(16) << setw(7) << setfill('0') << ev1[0][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_b1 << setbase(16) << setw(7) << setfill('0') << ev1[1][1].m_values.get()->m_data[i+j].m_value; 
            extPro_evk1_b2 << setbase(16) << setw(7) << setfill('0') << ev1[2][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_b3 << setbase(16) << setw(7) << setfill('0') << ev1[3][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_b4 << setbase(16) << setw(7) << setfill('0') << ev1[4][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_b5 << setbase(16) << setw(7) << setfill('0') << ev1[5][1].m_values.get()->m_data[i+j].m_value; 
            extPro_evk1_b6 << setbase(16) << setw(7) << setfill('0') << ev1[6][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk1_b7 << setbase(16) << setw(7) << setfill('0') << ev1[7][1].m_values.get()->m_data[i+j].m_value;
        }
        extPro_evk1_a0 << "\n";
        extPro_evk1_a1 << "\n"; 
        extPro_evk1_a2 << "\n";
        extPro_evk1_a3 << "\n";
        extPro_evk1_a4 << "\n";
        extPro_evk1_a5 << "\n"; 
        extPro_evk1_a6 << "\n";
        extPro_evk1_a7 << "\n";  
        extPro_evk1_b0 << "\n";
        extPro_evk1_b1 << "\n"; 
        extPro_evk1_b2 << "\n";
        extPro_evk1_b3 << "\n";
        extPro_evk1_b4 << "\n";
        extPro_evk1_b5 << "\n"; 
        extPro_evk1_b6 << "\n";
        extPro_evk1_b7 << "\n";
    }
    extPro_evk1_a0.close();
    extPro_evk1_a1.close();
    extPro_evk1_a2.close();
    extPro_evk1_a3.close();
    extPro_evk1_a4.close();
    extPro_evk1_a5.close();
    extPro_evk1_a6.close();
    extPro_evk1_a7.close();
    extPro_evk1_b0.close();
    extPro_evk1_b1.close();
    extPro_evk1_b2.close();
    extPro_evk1_b3.close();
    extPro_evk1_b4.close();
    extPro_evk1_b5.close();
    extPro_evk1_b6.close();
    extPro_evk1_b7.close();


    // 1.1 temp1_a generation
    NativePoly temp1(dct[0] * ev1[0][0]);
    for (size_t l = 1; l < digitsG2; ++l)
        temp1 += (dct[l] * ev1[l][0]);

    std::ofstream temp1_a_file("/home/ove2/openfhe-development/CMUX_TEMP1_A.txt"); 
    for (int i = 0; i < 1024; i++) {
        temp1_a_file << temp1.m_values.get()->m_data[i].m_value << "\n";
    }
    temp1_a_file.close();
    NativePoly acc_inc_a = temp1 *= monomial;
    // acc->GetElements()[0] += (temp1 *= monomial);

    // 1.2 temp1_b generation
    temp1 = dct[0] * ev1[0][1];
    for (size_t l = 1; l < digitsG2; ++l)
        temp1 += (dct[l] * ev1[l][1]);

    std::ofstream temp1_b_file("/home/ove2/openfhe-development/CMUX_TEMP1_B.txt"); 
    for (int i = 0; i < 1024; i++) {
        temp1_b_file << temp1.m_values.get()->m_data[i].m_value << "\n";
    }
    temp1_b_file.close();

    NativePoly acc_inc_b = temp1 *= monomial;
    // acc->GetElements()[1] += (temp1 *= monomial);

    const std::vector<std::vector<NativePoly>>& ev2 = ek2->GetElements();
    //  get data of evk2 input of externalProduct
    std::ofstream extPro_evk2_a0("/home/ove2/openfhe-development/ExtPro_EVK2_A0.txt"); 
    std::ofstream extPro_evk2_a1("/home/ove2/openfhe-development/ExtPro_EVK2_A1.txt"); 
    std::ofstream extPro_evk2_a2("/home/ove2/openfhe-development/ExtPro_EVK2_A2.txt"); 
    std::ofstream extPro_evk2_a3("/home/ove2/openfhe-development/ExtPro_EVK2_A3.txt");
    std::ofstream extPro_evk2_a4("/home/ove2/openfhe-development/ExtPro_EVK2_A4.txt"); 
    std::ofstream extPro_evk2_a5("/home/ove2/openfhe-development/ExtPro_EVK2_A5.txt"); 
    std::ofstream extPro_evk2_a6("/home/ove2/openfhe-development/ExtPro_EVK2_A6.txt"); 
    std::ofstream extPro_evk2_a7("/home/ove2/openfhe-development/ExtPro_EVK2_A7.txt");  
    std::ofstream extPro_evk2_b0("/home/ove2/openfhe-development/ExtPro_EVK2_B0.txt"); 
    std::ofstream extPro_evk2_b1("/home/ove2/openfhe-development/ExtPro_EVK2_B1.txt"); 
    std::ofstream extPro_evk2_b2("/home/ove2/openfhe-development/ExtPro_EVK2_B2.txt"); 
    std::ofstream extPro_evk2_b3("/home/ove2/openfhe-development/ExtPro_EVK2_B3.txt"); 
    std::ofstream extPro_evk2_b4("/home/ove2/openfhe-development/ExtPro_EVK2_B4.txt"); 
    std::ofstream extPro_evk2_b5("/home/ove2/openfhe-development/ExtPro_EVK2_B5.txt"); 
    std::ofstream extPro_evk2_b6("/home/ove2/openfhe-development/ExtPro_EVK2_B6.txt"); 
    std::ofstream extPro_evk2_b7("/home/ove2/openfhe-development/ExtPro_EVK2_B7.txt"); 
    for (int i = 0; i < 1024; i += 8) {
        for (int j = 7; j >= 0; j--) {
            extPro_evk2_a0 << setbase(16) << setw(7) << setfill('0') << ev2[0][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_a1 << setbase(16) << setw(7) << setfill('0') << ev2[1][0].m_values.get()->m_data[i+j].m_value; 
            extPro_evk2_a2 << setbase(16) << setw(7) << setfill('0') << ev2[2][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_a3 << setbase(16) << setw(7) << setfill('0') << ev2[3][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_a4 << setbase(16) << setw(7) << setfill('0') << ev2[4][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_a5 << setbase(16) << setw(7) << setfill('0') << ev2[5][0].m_values.get()->m_data[i+j].m_value; 
            extPro_evk2_a6 << setbase(16) << setw(7) << setfill('0') << ev2[6][0].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_a7 << setbase(16) << setw(7) << setfill('0') << ev2[7][0].m_values.get()->m_data[i+j].m_value;  
            extPro_evk2_b0 << setbase(16) << setw(7) << setfill('0') << ev2[0][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_b1 << setbase(16) << setw(7) << setfill('0') << ev2[1][1].m_values.get()->m_data[i+j].m_value; 
            extPro_evk2_b2 << setbase(16) << setw(7) << setfill('0') << ev2[2][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_b3 << setbase(16) << setw(7) << setfill('0') << ev2[3][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_b4 << setbase(16) << setw(7) << setfill('0') << ev2[4][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_b5 << setbase(16) << setw(7) << setfill('0') << ev2[5][1].m_values.get()->m_data[i+j].m_value; 
            extPro_evk2_b6 << setbase(16) << setw(7) << setfill('0') << ev2[6][1].m_values.get()->m_data[i+j].m_value;
            extPro_evk2_b7 << setbase(16) << setw(7) << setfill('0') << ev2[7][1].m_values.get()->m_data[i+j].m_value;
        }
        extPro_evk2_a0 << "\n";
        extPro_evk2_a1 << "\n"; 
        extPro_evk2_a2 << "\n";
        extPro_evk2_a3 << "\n";
        extPro_evk2_a4 << "\n";
        extPro_evk2_a5 << "\n"; 
        extPro_evk2_a6 << "\n";
        extPro_evk2_a7 << "\n";  
        extPro_evk2_b0 << "\n";
        extPro_evk2_b1 << "\n"; 
        extPro_evk2_b2 << "\n";
        extPro_evk2_b3 << "\n";
        extPro_evk2_b4 << "\n";
        extPro_evk2_b5 << "\n"; 
        extPro_evk2_b6 << "\n";
        extPro_evk2_b7 << "\n";
    }
    extPro_evk2_a0.close();
    extPro_evk2_a1.close();
    extPro_evk2_a2.close();
    extPro_evk2_a3.close();
    extPro_evk2_a4.close();
    extPro_evk2_a5.close();
    extPro_evk2_a6.close();
    extPro_evk2_a7.close();
    extPro_evk2_b0.close();
    extPro_evk2_b1.close();
    extPro_evk2_b2.close();
    extPro_evk2_b3.close();
    extPro_evk2_b4.close();
    extPro_evk2_b5.close();
    extPro_evk2_b6.close();
    extPro_evk2_b7.close();



    // 2.1 temp2_a generation for elements[0]:
    NativePoly temp2(dct[0] * ev2[0][0]);
    for (size_t l = 1; l < digitsG2; ++l)
        temp2 += (dct[l] * ev2[l][0]);

    std::ofstream temp2_a_file("/home/ove2/openfhe-development/CMUX_TEMP2_A.txt"); 
    for (int i = 0; i < 1024; i++) {
        temp2_a_file << temp2.m_values.get()->m_data[i].m_value << "\n";
    }
    temp2_a_file.close();
    
    acc_inc_a += (temp2 *= monomialNeg);
    // acc->GetElements()[0] += (temp2 *= monomialNeg);
    
    //  2.2 temp2_b generation for elements[1]:
    temp2 = dct[0] * ev2[0][1];
    for (size_t l = 1; l < digitsG2; ++l)
        temp2 += (dct[l] *= ev2[l][1]);

    std::ofstream temp2_b_file("/home/ove2/openfhe-development/CMUX_TEMP2_B.txt"); 
    for (int i = 0; i < 1024; i++) {
        temp2_b_file << temp2.m_values.get()->m_data[i].m_value << "\n";
    }
    temp2_b_file.close();

    acc_inc_b += (temp2 *= monomialNeg);
    // acc->GetElements()[1] += (temp2 *= monomialNeg);

    std::ofstream acc_inc_a_file("/home/ove2/openfhe-development/CMUX_ACC_INC_A.txt"); 
    std::ofstream acc_inc_b_file("/home/ove2/openfhe-development/CMUX_ACC_INC_B.txt"); 
    for (int i = 0; i < 1024; i++) {
        acc_inc_a_file << acc_inc_a.m_values.get()->m_data[i].m_value << "\n";
        acc_inc_b_file << acc_inc_b.m_values.get()->m_data[i].m_value << "\n";
    }
    acc_inc_a_file.close();
    acc_inc_b_file.close();

    acc->GetElements()[0] += acc_inc_a;
    acc->GetElements()[1] += acc_inc_b;
}

};  // namespace lbcrypto
