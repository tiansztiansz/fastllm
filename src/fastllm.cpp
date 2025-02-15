//
// Created by huangyuyang on 5/11/23.
//

#include "utils.h"

#include "fastllm.h"

#include "executor.h"

#include <cstring>
#include <cmath>
#include <cfloat>
#include <thread>

#ifdef __aarch64__
#include <arm_neon.h>
#include "armMath.h"
#endif

#ifdef __AVX__
#include "immintrin.h"
#endif

#ifdef USE_CUDA
#include "fastllm-cuda.h"
#endif

namespace fastllm {
    Executor defaultExecutor;
    Executor *curExecutor = &defaultExecutor;

    static int threads = 4;
    static bool lowMemMode = false;
    static bool kvCacheInCPU = false;

    void SetKVCacheInCPU(bool v) {
        kvCacheInCPU = v;
    }

    void SetThreads(int t) {
        threads = t;
    }

    void SetLowMemMode(bool m) {
    	lowMemMode = m;
    }

    bool GetKVCacheInCPU() {
        return kvCacheInCPU;
    }

    bool GetLowMemMode() {
        return lowMemMode;
    }

    int GetThreads() {
        return threads;
    }

    struct FileBuffer {
        FILE *f;

        FileBuffer (const std::string &fileName) {
            f = fopen(fileName.c_str(), "rb");
        }

        int ReadInt() {
            int v;
            if (fread(&v, 1, 4, f) != 4) {
                ErrorInFastLLM("FileBuffer.ReadInt error.\n");
            };
            return v;
        }

        float ReadFloat() {
            float v;
            if (fread(&v, 1, 4, f) != 4) {
                ErrorInFastLLM("FileBuffer.ReadFloat error.\n");
            };
            return v;
        }

        std::string ReadString() {
            int len = ReadInt();
            std::string ret = "";
            char *v = new char[len + 5];
            v[len] = 0;
            if (fread(v, 1, len, f) != len) {
                ErrorInFastLLM("FileBuffer.ReadString error.\n");
            }
            return v;
        }

        void ReadBytes(uint8_t *buffer, uint64_t bytes) {
            if (fread(buffer, 1, bytes, f) != bytes) {
                ErrorInFastLLM("FileBuffer.ReadBytes error.\n");
            }
        }

        ~FileBuffer() {
            fclose(f);
        }
    };

    struct FileWriter {
        FILE *f;

        FileWriter (const std::string &fileName) {
            f = fopen(fileName.c_str(), "wb");
        }

        void WriteInt(int v) {
            if (fwrite(&v, 1, 4, f) != 4) {
                ErrorInFastLLM("FileWriter.WriteInt error.\n");
            };
        }

        void WriteFloat(float v) {
            if (fwrite(&v, 1, 4, f) != 4) {
                ErrorInFastLLM("FileWriter.WriteFloat error.\n");
            };
        }

        void WriteString(const std::string &s) {
            WriteInt((int)s.size());
            if (fwrite(s.c_str(), 1, (int)s.size(), f) != (int)s.size()) {
                ErrorInFastLLM("FileWriter.WriteString Error.\n");
            }
        }

        void WriteBytes(uint8_t *buffer, uint64_t bytes) {
            if (fwrite(buffer, 1, bytes, f) != bytes) {
                ErrorInFastLLM("FileWriter.WriteBytes error.\n");
            }
        }

        ~FileWriter() {
            fclose(f);
        }
    };

    Data::Data(fastllm::DataType type) {
        this->dataType = type;
        this->UpdateUnitSize();
    }

    Data::Data(fastllm::DataType type, const std::vector<int> &dims) {
        this->dataType = type;
        Resize(dims);
    }

    Data::Data(fastllm::DataType type, const std::vector<int> &dims, const std::vector<float> &data) : Data::Data(type, dims) {
        this->Allocate();
        if (type == DataType::FLOAT32) {
            std::memcpy(this->cpuData, data.data(), this->GetBytes());
        }
    }

    Data::Data(const Data &ori) {
        CopyFrom(ori);
    }

    void Data::CopyFrom(const Data &ori) {
        if (ori.dims != this->dims || this->cpuData == nullptr) {
            if (ori.dims.size() == 0) {
                delete[] this->cpuData;
                this->dataType = ori.dataType;
                this->UpdateUnitSize();
                this->dims.resize(0);
                this->cpuData = nullptr;
                return;
            }
            this->dataType = ori.dataType;
            this->Resize(ori.dims);
            this->Allocate();
        }
        std::memcpy(this->cpuData, ori.cpuData, this->GetBytes());
    }

    uint64_t Data::Count(int i) const {
        if (i >= this->dims.size()) {
            return 1;
        }
        if (i - 1 >= 0 && i - 1 < this->strides.size()) {
            return this->strides[i - 1];
        }
        return this->dims[i] * this->strides[i];
    }

    void Data::UpdateUnitSize() {
        if (this->dataType == DataType::FLOAT32) {
            this->unitSize = 4;
            this->unitSizeDiv = 1;
        } else if (this->dataType == DataType::BFLOAT16 ||
                this->dataType == DataType::INT16 ||
                this->dataType == DataType::FLOAT16) {
            this->unitSize = 2;
            this->unitSizeDiv = 1;
        } else if (this->dataType == DataType::INT8) {
            this->unitSize = 1;
            this->unitSizeDiv = 1;
        } else if (this->dataType == DataType::INT4) {
            this->unitSize = 1;
            this->unitSizeDiv = 2;
        } else if (this->dataType == DataType::INT2) {
            this->unitSize = 1;
            this->unitSizeDiv = 4;
        } else if (this->dataType == DataType::BIT) {
            this->unitSize = 1;
            this->unitSizeDiv = 8;
        } else if (this->dataType == DataType::INT32PARAM) {
            this->unitSize = 4;
            this->unitSizeDiv = 1;
        }
    }

    void Data::Resize(const std::vector<int> &dims) {
        this->dims = dims;
        this->UpdateUnitSize();

        if (this->expansionDims.size() == 0) {
            this->strides.resize(dims.size(), 1);
            this->strides.back() = 1;
            for (int i = this->dims.size() - 2; i >= 0; i--) {
                this->strides[i] = this->dims[i + 1] * this->strides[i + 1];
            }
        }
    }

    void Data::Reshape(const std::vector<int> &dims) {
        std::vector <int> outputDims = dims;
        uint64_t old = 1;
        for (int i : this->dims) {
            old *= i;
        }
        int index = -1;
        uint64_t mul = 1;
        for (int i = 0; i < dims.size(); i++) {
            if (dims[i] < 0) {
                if (index == -1) {
                    index = i;
                } else {
                    ErrorInFastLLM("Reshape error.\n");
                }
            } else {
                mul *= dims[i];
            }
        }
        outputDims = dims;
        if (index == -1) {
            AssertInFastLLM(mul == old, "Reshape error.\n");
        } else {
            AssertInFastLLM(mul != 0, "Reshape error.\n");
            AssertInFastLLM(old % mul == 0, "Reshape error.\n");
            outputDims[index] = old / mul;
        }
        Resize(outputDims);
    }

    uint64_t Data::GetBytes() const {
        return (this->strides[0] * this->dims[0] * this->unitSize - 1) / this->unitSizeDiv + 1;
    }

    void Data::MallocSpace(uint64_t size) {
        this->expansionSize = size;
        this->expansionBytes = (size * this->unitSize - 1) / this->unitSizeDiv + 1;
        if (this->dataDevice == DataDevice::CPU) {
            this->cpuData = new uint8_t[this->expansionBytes];
        } else if (this->dataDevice == DataDevice::CUDA) {
#ifdef USE_CUDA
            this->cudaData = FastllmCudaMalloc(this->expansionBytes);
#else
            ErrorInFastLLM("Error: cuda is not supported.\n");
#endif
        }
    }

    void Data::FreeSpace() {
        this->expansionSize = 0;
        this->expansionBytes = 0;
        if (this->dataDevice == DataDevice::CPU) {
            delete[] this->cpuData;
        } else if (this->dataDevice == DataDevice::CUDA) {
#ifdef USE_CUDA
            FastllmCudaFree(this->cudaData);
#else
            ErrorInFastLLM("Error: cuda is not supported.\n");
#endif
        }
    }

    void Data::Allocate() {
        if (Count(0) > expansionSize) {
            FreeSpace();
            MallocSpace(Count(0));
        }
    }

    void Data::Allocate(float v) {
        AssertInFastLLM(this->dataType == DataType::FLOAT32, "Allocate error: Data's type should be float32.\n");
        this->Allocate();
        float *f = (float*)cpuData;
        if (this->dataDevice == DataDevice::CPU) {
            std::fill(f, f + Count(0), v);
        } else {
            // TODO: 别的设备上的初始化
        }
    }

    void Data::Expansion(const std::vector<int> &dims) {
        if (this->dims.size() == 0) {
            this->strides.resize(dims.size(), 1);
            this->strides.back() = 1;
            for (int i = dims.size() - 2; i >= 0; i--) {
                this->strides[i] = dims[i + 1] * this->strides[i + 1];
            }
            this->expansionDims = dims;
            this->MallocSpace(this->strides[0] * dims[0]);
            return;
        }

        AssertInFastLLM(dims.size() == this->dims.size(), "Expansion error: real dims's size should equal to expansion dims's size.\n");
        for (int i = 0; i < dims.size(); i++) {
            AssertInFastLLM(dims[i] == -1 || dims[i] >= this->dims[i], "Expansion error: real size should <= expansion size.\n");
        }

        int axis = -1;
        for (int i = 0; i < this->dims.size(); i++) {
            if (this->dims[i] < dims[i]) {
                axis = i;
                break;
            }
        }

        uint64_t oldBytes = GetBytes();
        int input1Stride = this->Count(axis);

        this->strides.resize(dims.size(), 1);
        this->strides.back() = 1;
        for (int i = this->dims.size() - 2; i >= 0; i--) {
            this->strides[i] = std::max(this->dims[i + 1], dims[i + 1]) * this->strides[i + 1];
        }
        this->expansionDims = dims;
        if (this->expansionBytes != 0) {
            if (this->dataDevice == DataDevice::CPU) {
                uint8_t *old = this->cpuData;
                MallocSpace(this->strides[0] * std::max(this->dims[0], dims[0]));
                int outer = this->Count(0) / this->Count(axis);
                int input0Stride = this->Count(axis);
                int inner = this->strides[axis];
                int unitSize = this->unitSize;
                for (int o = 0; o < outer; o++) {
                    memcpy(this->cpuData + o * input0Stride * unitSize,
                           old + o * input1Stride * unitSize,
                           this->dims[axis] * inner * unitSize);
                }
                delete[] old;
            } else if (this->dataDevice == DataDevice::CUDA) {
#ifdef USE_CUDA
                uint8_t *old = (uint8_t*)this->cudaData;
                MallocSpace(this->strides[0] * std::max(this->dims[0], dims[0]));
                int outer = this->Count(0) / this->Count(axis);
                int input0Stride = this->Count(axis);
                int inner = this->strides[axis];
                int unitSize = this->unitSize;
                FastllmCudaMemcpy2DDeviceToDevice((uint8_t*)this->cudaData, input0Stride * unitSize,
                                            (uint8_t*)old, input1Stride * unitSize, this->dims[axis] * inner * unitSize, outer);
                FastllmCudaFree(old);
#else
                ErrorInFastLLM("Error: cuda is not supported.\n");
#endif
            }
        } else {
            MallocSpace(this->strides[0] * std::max(this->dims[0], dims[0]));
        }
    }

    Data::~Data() {
        delete[] this->cpuData;
#ifdef USE_CUDA
        if (this->cudaData != nullptr) {
            FastllmCudaFree(this->cudaData);
        }
#endif
    }

    void Data::PrintShape() const {
        printf("shape: ");
        for (int i : this->dims) {
            printf("%d ", i);
        }
        printf("\n");
    }

    void Data::Print() const {
        printf("shape: ");
        for (int i : this->dims) {
            printf("%d ", i);
        }
        printf("\ndata: ");
        /*
        int len = Count(0);
        if (len < 20) {
            for (int i = 0; i < len; i++) {
                printf("%f ", ((float*)cpuData)[i]);
            }
        } else {
            for (int i = 0; i < 10; i++) {
                printf("%f ", ((float *) cpuData)[i]);
            }
            printf("... ");
            for (int i = 0; i < 10; i++) {
                printf("%f ", ((float *) cpuData)[len - 10 + i]);
            }
        }
        printf("\n");
         */
        int n = Count(0) / dims.back(), m = dims.back();
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 10 && j < m; j++) {
                printf("%f ", ((float*)cpuData)[i * m + j]);
            }
            if (m > 10) {
                printf("... ");
                for (int j = 0; j < 10 && j < m; j++) {
                    printf("%f ", ((float *) cpuData)[i * m + (m - 10 + j)]);
                }
            }
            printf("\n");
        }
    }

    void Data::CalcWeightSum() {
        if (this->weightSum.size() > 0) {
            return;
        }
        int n = this->dims[0], m = this->dims[1];
        if (this->dataType == DataType::INT8) {
            weightSum.resize(n);
            for (int i = 0; i < n; i++) {
                int j = 0;
#ifdef __AVX__
                __m256i acc = _mm256_setzero_si256();
                const __m256i ones = _mm256_set1_epi16(1);
                for (; j + 31 < m; j += 32) {
                    __m256i ax = _mm256_loadu_si256((const __m256i *) (cpuData + i * m + j));
                    __m256i mx0 = _mm256_cvtepu8_epi16(_mm256_extractf128_si256(ax, 0));
                    __m256i mx1 = _mm256_cvtepu8_epi16(_mm256_extractf128_si256(ax, 1));
                    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(mx0, ones));
                    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(mx1, ones));
                }
                weightSum[i] += I32sum(acc);
#endif
#ifdef __aarch64__
                uint32x4_t sum0 = {0, 0, 0, 0};
                for (; j + 7 < m; j += 8) {
                    uint8x8_t ori = vld1_u8(cpuData + (i * m + j));
                    uint16x4_t sa = vpaddl_u8 (ori);
                    sum0 = vaddw_u16(sum0, sa);
                }
                weightSum[i] += sum0[0] + sum0[1] + sum0[2] + sum0[3];
#endif
                for (; j < m; j++) {
                    weightSum[i] += cpuData[i * m + j];
                }
            }
        } else if (this->dataType == DataType::INT4) {
            weightSum.resize(n);
            for (int i = 0; i < n; i++) {
                int j = 0;
#ifdef __aarch64__
                uint8x8_t maskHigh = vdup_n_u8(0xF0);
                uint8x8_t maskLow = vdup_n_u8(0xF);
                uint32x4_t sum0 = {0, 0, 0, 0};

                for (; j + 15 < m; j += 16) {
                    uint8x8_t ori = vld1_u8(cpuData + (i * m + j) / 2);
                    uint8x8_t va = vand_u8(ori, maskLow);
                    uint8x8_t vb = vshr_n_u8(vand_u8(ori, maskHigh), 4);

                    uint16x4_t sa = vpaddl_u8 (va);
                    uint16x4_t sb = vpaddl_u8 (vb);

                    sum0 = vaddw_u16(sum0, vadd_u16(sa, sb));
                }
                weightSum[i] += sum0[0] + sum0[1] + sum0[2] + sum0[3];
#endif
#ifdef __AVX__
	            __m256i acc = _mm256_setzero_si256();
	            const __m256i lowMask = _mm256_set1_epi8(0xf);
	            const __m256i ones = _mm256_set1_epi16(1);
	            for (; j + 31 < m; j += 32) {
		            __m128i orix = _mm_loadu_si128((const __m128i *) (cpuData + (i * m + j) / 2));
		            __m256i bytex = _mm256_set_m128i(_mm_srli_epi16(orix, 4), orix);
		            __m256i bx = _mm256_and_si256(lowMask, bytex);

		            __m256i mx0 = _mm256_cvtepu8_epi16(_mm256_extractf128_si256(bx, 0));
		            __m256i mx1 = _mm256_cvtepu8_epi16(_mm256_extractf128_si256(bx, 1));

		            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(mx0, ones));
		            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(mx1, ones));
	            }
	            weightSum[i] += I32sum(acc);
#endif
                for (; j + 1 < m; j += 2) {
	                int id = (i * m + j) / 2;
	                weightSum[i] += (cpuData[id] & 0xF) + (cpuData[id] >> 4);
                }
                for (; j < m; j++) {
                    int id = (i * m + j) / 2;
                    if ((i * m + j) % 2) {
                        weightSum[i] += (cpuData[id] & 0xF);
                    } else {
                        weightSum[i] += (cpuData[id] >> 4);
                    }
                }
            }
        }
    }

    void Data::ToDevice(void *device) {
        BaseDevice *dev = (BaseDevice*)device;
        if (dev->deviceType == "cuda") {
            this->ToDevice(DataDevice::CUDA);
        } else {
            this->ToDevice(DataDevice::CPU);
        }
    }

    void Data::ToDevice(fastllm::DataDevice device) {
        if (this->dataType == DataType::INT32PARAM) {
            return;
        }
#ifndef USE_CUDA
        // TODO: 这里先直接跳过了
        return;
#endif
        if (this->dataDevice == device) {
            return;
        }

        if (this->expansionBytes != 0) {
#ifdef USE_CUDA
            if (this->dataDevice == DataDevice::CPU) {
                if (device == DataDevice::CUDA) {
                    this->cudaData = FastllmCudaMalloc(expansionBytes);
                    FastllmCudaCopyFromHostToDevice(this->cudaData, this->cpuData, expansionBytes);
                    delete[] this->cpuData;
                    this->cpuData = nullptr;
                }
            } else if (this->dataDevice == DataDevice::CUDA) {
                if (device == DataDevice::CPU) {
                    this->cpuData = new uint8_t[expansionBytes];
                    FastllmCudaCopyFromDeviceToHost(this->cpuData, this->cudaData, expansionBytes);
                    FastllmCudaFree(this->cudaData);
                    this->cudaData = nullptr;
                }
            }
#endif
        }
        this->dataDevice = device;
    }

    Tokenizer::TrieNode::TrieNode() {
        this->tokenId = -999999;
    }

    Tokenizer::Tokenizer() {
        root = new TrieNode();
    }

    Tokenizer::~Tokenizer() {
        Clear();
        delete root;
    }

    void Tokenizer::Clear() {
        std::vector <TrieNode*> q;
        q.push_back(root);
        for (int i = 0; i < q.size(); i++) {
            TrieNode *now = q[i];
            for (auto it : now->next) {
                q.push_back(it.second);
            }
        }
        root = new TrieNode();
        tokenToStringDict.clear();
    }

    void Tokenizer::Insert(const std::string &s, int tokenId) {
        TrieNode *now = this->root;
        for (int i = 0; i < s.size(); i++) {
            if (now->next.find(s[i]) == now->next.end()) {
                now->next[s[i]] = new TrieNode();
            }
            now = now->next[s[i]];
        }
        now->tokenId = tokenId;
        tokenToStringDict[tokenId] = s;
    }

    Data Tokenizer::Encode(const std::string &s) {
        std::vector <float> v;
        for (int i = 0; i < s.size(); i++) {
            int tokenId = -999999, pos = i - 1;
            TrieNode *now = this->root;
            for (int j = i; j < s.size(); j++) {
                if (now->next.find(s[j]) != now->next.end()) {
                    now = now->next[s[j]];
                    if (now->tokenId != -999999) {
                        tokenId = now->tokenId;
                        pos = j;
                    }
                } else {
                    break;
                }
            }
            if (pos >= i) {
                i = pos;
                v.push_back(tokenId);
                //printf("%d ", tokenId);
            }
        }
        //printf("\n");

        return Data (DataType::FLOAT32, {1, (int)v.size()}, v);
    }

    std::string Tokenizer::Decode(const Data &data) {
        std::string ret = "";
        for (int i = 0; i < data.Count(0); i++) {
            std::string &s = tokenToStringDict[(int) ((float *) data.cpuData)[i]];
            if (s == "<n>") {
                ret += "\n";
            } else if (s == "<|tab|>") {
                s = "\t";
            } else {
                ret += s;
            }
        }

        std::string blank = "";
        blank += 226, blank += 150, blank += 129;
        while (true) {
            std::string::size_type pos(0);
            if ((pos = ret.find(blank)) != std::string::npos)
                ret.replace(pos, blank.length(), " ");
            else break;
        }
	    int pos = ret.find("<|blank_");
	    if (pos != -1) {
		    int space_num = atoi(ret.substr(8, ret.size() - 10).c_str());
		    return std::string(space_num, ' ');
	    }
        if (ret.size() == 6 && ret.substr(0, 3) == "<0x" && ret.back() == '>') {
            int c = 0;
            for (int i = 3; i < 5; i++) {
                c *= 16;
                if (ret[i] >= '0' && ret[i] <= '9') {
                    c += (ret[i] - '0');
                } else {
                    c += (ret[i] - 'A' + 10);
                }
            }

            ret = " ";
            ret[0] = c;
        }
        return ret;
    }

    void WeightMap::LoadFromFile(const std::string &fileName) {
        FileBuffer buffer(fileName);
        this->versionId = buffer.ReadInt();

        if (this->versionId == 1) {
            // versionId = 1, 前置了一个key-value表
            int keyValueLen = buffer.ReadInt();
            for (int i = 0; i < keyValueLen; i++) {
                std::string key = buffer.ReadString();
                std::string value = buffer.ReadString();
                //printf("%s %s\n", key.c_str(), value.c_str());
                this->dicts[key] = value;
            }
        }

        int vocabLen = buffer.ReadInt();
        for (int i = 0; i < vocabLen; i++) {
            int len = buffer.ReadInt();
            std::string x = "";
            for (int j = 0; j < len; j++) {
                x += buffer.ReadInt();
            }
            int id = buffer.ReadInt();
            tokenizer.Insert(x, id);
        }

        int len = buffer.ReadInt();
        for (int i = 0; i < len; i++) {
            std::string name = buffer.ReadString();
            //printf("%s\n", name.c_str());
            int dimsSize = buffer.ReadInt();
            //printf("size = %d\n", dimsSize);
            std::vector <int> dims;
            for (int j = 0; j < dimsSize; j++) {
                int x = buffer.ReadInt();
                dims.push_back(x);
                //printf("%d\n", x);
            }
            DataType dataType = (DataType)buffer.ReadInt();
            weight[name] = Data(dataType, dims);

            if (lowMemMode && this->embeddingNames.find(name) != this->embeddingNames.end()) {
	            if (dataType == DataType::FLOAT32 || dataType == DataType::BFLOAT16 || dataType == DataType::FLOAT16) {
	            	weight[name].fileName = fileName;
#if defined(_WIN32) or defined(_WIN64)
	            	weight[name].filePos = _ftelli64(buffer.f);
#else
                    weight[name].filePos = ftell(buffer.f);
#endif
	            	fseek(buffer.f, weight[name].GetBytes(), SEEK_CUR);
	            } else {
	            	ErrorInFastLLM("Error: embedding's type should be float32 or bfloat16.\n");
	            }
            } else {
	            weight[name].Allocate();
	            if (dataType == DataType::FLOAT32 || dataType == DataType::BFLOAT16 || dataType == DataType::FLOAT16) {
		            buffer.ReadBytes(weight[name].cpuData, weight[name].GetBytes());
	            } else if (dataType == DataType::INT8 || dataType == DataType::INT4) {
		            int bit = (dataType == DataType::INT4 ? 4 : 8);
		            weight[name].perChannelAxis = buffer.ReadInt();
		            int k = weight[name].perChannelAxis == -1 ? 1 : dims[weight[name].perChannelAxis];
		            weight[name].perChannelsConfigs.resize(k);
		            weight[name].zeros.resize(k);
		            weight[name].scales.resize(k);
		            for (int i = 0; i < k; i++) {
			            float minValue = buffer.ReadFloat();
			            float maxValue = buffer.ReadFloat();
			            weight[name].perChannelsConfigs[i] = LowBitConfig(minValue, maxValue, bit);
			            weight[name].zeros[i] = weight[name].perChannelsConfigs[i].zeroPoint;
			            weight[name].scales[i] = weight[name].perChannelsConfigs[i].scale;
		            }
		            buffer.ReadBytes(weight[name].cpuData, weight[name].GetBytes());
	            }
            }

            printf("Load (%d / %d) \r", (i + 1), len);
            fflush(stdout);
        }
        printf("\n");
        fflush(stdout);
        return;
    }

    void WeightMap::SaveLowBitModel(const std::string &fileName, int bit) {
        AssertInFastLLM(fileName != "", "Error: output's name shouldn't be empty.\n");
        AssertInFastLLM(bit == 4 || bit == 8 || bit == 16, "Error: only support 16 bit or 8 bit or 4 bit model.\n");
        FileWriter buffer(fileName);
        buffer.WriteInt(this->versionId);
        if (this->versionId == 1) {
            // versionId = 1, 前置了一个key-value表
            buffer.WriteInt((int)dicts.size());
            for (auto &it : dicts) {
                buffer.WriteString(it.first);
                buffer.WriteString(it.second);
            }
        }

        // 写入词表
        buffer.WriteInt((int)tokenizer.tokenToStringDict.size());
        for (auto &it : tokenizer.tokenToStringDict) {
            buffer.WriteInt((int)it.second.size());
            for (int i = 0; i < it.second.size(); i++) {
                buffer.WriteInt((int)it.second[i]);
            }
            buffer.WriteInt(it.first);
        }

        // 写入权重
        buffer.WriteInt((int)weight.size());
        for (auto &it : weight) {
            buffer.WriteString(it.first);
            Data &data = it.second;
            buffer.WriteInt((int)data.dims.size());
            for (int i : data.dims) {
                buffer.WriteInt(i);
            }
            data.ToDevice(DataDevice::CPU);

            if (data.weightType == WeightType::NONE) {
                // 普通权重，直接写入浮点数据
                buffer.WriteInt((int)DataType::FLOAT32);
                buffer.WriteBytes(data.cpuData, data.GetBytes());
            } else if (data.weightType == WeightType::EMBEDDING) {
                // Embedding权重，存储成BF16
                buffer.WriteInt((int)DataType::BFLOAT16);
                int len = data.Count(0);
                std::vector <uint16_t> uDatas;
                uDatas.resize(len);
                for (int i = 0; i < len; i++) {
                    uDatas[i] = ((uint16_t *)data.cpuData)[i * 2 + 1];
                }
                buffer.WriteBytes((uint8_t*)uDatas.data(), len * sizeof(uint16_t));
            } else if (data.weightType == WeightType::LINEAR) {
                if (bit == 16) {
                    // fp16, 直接转换
                    buffer.WriteInt((int)DataType::FLOAT16);
                    int len = data.Count(0);
                    std::vector <uint16_t> uDatas;
                    uDatas.resize(len);
                    for (int i = 0; i < len; i++) {
                        uDatas[i] = float_to_half(((float *)data.cpuData)[i]);
                    }
                    buffer.WriteBytes((uint8_t*)uDatas.data(), len * sizeof(uint16_t));
                } else {
                    // Linear层权重，分通道量化之
                    int k = data.dims[0], m = data.dims[1];
                    int threadNum = 8;
                    int per = k / threadNum;
                    int cur = 0;
                    std::vector<std::thread *> threads;
                    std::vector<LowBitConfig> configs;
                    std::vector<uint8_t> uDatas;
                    configs.resize(k);

                    int bytes = k * m;
                    if (bit == 4) {
                        bytes = (k * m + 1) / 2;
                    }
                    uDatas.resize(bytes);
                    for (int i = 0; i < threadNum; i++) {
                        int end = cur + per;
                        if (i == threadNum - 1) {
                            end = k;
                        }
                        threads.push_back(new std::thread([&bit](int st, int end, int m,
                                                                 float *f, uint8_t *u8, LowBitConfig *configs) {
                            for (int i = st; i < end; i++) {
                                float minValue = 1e9, maxValue = -1e9;
                                for (int j = 0; j < m; j++) {
                                    minValue = std::min(minValue, f[i * m + j]);
                                    maxValue = std::max(maxValue, f[i * m + j]);
                                }
                                if (bit == 8) {
                                    configs[i] = LowBitConfig(minValue, maxValue, 8);
                                    for (int j = 0; j < m; j++) {
                                        u8[i * m + j] = configs[i].quantization(f[i * m + j]);
                                    }
                                } else {
                                    configs[i] = LowBitConfig(minValue, maxValue, 4);
                                    for (int j = 0; j < m; j++) {
                                        int id = (i * m + j) / 2;
                                        uint8_t value = configs[i].quantization(f[i * m + j]);
                                        if ((i * m + j) % 2) {
                                            u8[id] = (u8[id] & 0xF0) | value;
                                        } else {
                                            u8[id] = (u8[id] & 0xF) | (value << 4);
                                        }
                                    }
                                }
                            }
                        }, cur, end, m, (float *) data.cpuData, uDatas.data(), configs.data()));
                        cur = end;
                    }
                    for (int i = 0; i < threadNum; i++) {
                        threads[i]->join();
                        delete threads[i];
                    }

                    buffer.WriteInt(bit == 8 ? (int) DataType::INT8 : (int) DataType::INT4);
                    buffer.WriteInt(0); // 按通道0分通道量化
                    for (int i = 0; i < k; i++) {
                        buffer.WriteFloat(configs[i].min);
                        buffer.WriteFloat(configs[i].max);
                    }
                    buffer.WriteBytes(uDatas.data(), bytes);
                }
            }
        }

        return;
    }

    Data &WeightMap::operator[](const std::string &key) {
        return weight[key];
    }

    void TokenPenaltyManager::Init(int vocabSize, int lastN, float value) {
        this->vocabSize = vocabSize;
        this->lastN = lastN;
        this->value = value;
        this->Clear();
    }

    void TokenPenaltyManager::Clear() {
        cnt.clear();
        while (!q.empty()) {
            q.pop();
        }
        penalty.CopyFrom(Data(DataType::FLOAT32, {1, 1, vocabSize}, std::vector <float> (vocabSize, 1.0f)));
    }

    void TokenPenaltyManager::InsertToken(int token) {
        if (q.size() >= this->lastN) {
            int now = q.front();
            if ((--cnt[now]) == 0) {
                cnt.erase(now);
                ((float*)penalty.cpuData)[now] = 1.0f;
            }
            q.pop();
        }

        q.push(token);
        if ((++cnt[token]) == 1) {
            ((float *) penalty.cpuData)[token] = this->value;
        }
    }

    void Embedding(const Data &input, Data &weight, Data &output) {
        curExecutor->Run("Embedding", {
                {"input", (Data*)&input}, {"weight", &weight}, {"output", &output}
        }, {}, {});
    }

    void RMSNorm(const Data &input, const Data &weight, float eps, Data &output) {
        curExecutor->Run("RMSNorm", {
                {"input", (Data*)&input}, {"weight", (Data*)&weight}, {"output", &output}
        }, {{"eps", eps}}, {});
    }

    void LayerNorm(Data &input, Data &gamma, Data &beta, int axis, Data &output) {
        curExecutor->Run("LayerNorm", {
            {"input", &input}, {"gamma", &gamma}, {"beta", &beta}, {"output", &output}
        }, {}, {{"axis", axis}});
    }

    void Linear(Data &input, Data &weight, const Data &bias, Data &output) {
        curExecutor->Run("Linear", {
                {"input", &input}, {"weight", &weight}, {"bias", (Data*)&bias}, {"output", &output}
        }, {}, {});
    }

    void Split(const Data &input, int axis, int start, int end, Data &output) {
        curExecutor->Run("Split", {
                {"input", (Data*)&input}, {"output", &output}
        }, {}, {{"axis", axis}, {"start", start}, {"end", end}});
    }

    void Cat(const Data &input0, const Data &input1, int axis, Data &output) {
        curExecutor->Run("Cat", {
                {"input0", (Data*)&input0}, {"input1", (Data*)&input1}, {"output", &output}
        }, {}, {{"axis", axis}});
    }

    void CatDirect(Data &input0, const Data &input1, int axis) {
        curExecutor->Run("CatDirect", {
                {"input0", (Data*)&input0}, {"input1", (Data*)&input1}
        }, {}, {{"axis", axis}});
    }

    void MatMul(const Data &input0, const Data &input1, Data &output, float alpha) {
        curExecutor->Run("MatMul", {
                {"input0", (Data*)&input0}, {"input1", (Data*)&input1}, {"output", &output}
        }, {{"alpha", alpha}}, {});
    }

    void MatMulTransB(const Data &input0, const Data &input1, Data &output, float alpha) {
        curExecutor->Run("MatMulTransB", {
                {"input0", (Data*)&input0}, {"input1", (Data*)&input1}, {"output", &output}
        }, {{"alpha", alpha}}, {});
    }

    void Softmax(const Data &input, Data &output, int axis) {
        curExecutor->Run("SoftMax", {
                {"input", (Data*)&input}, {"output", &output}
        }, {}, {{"axis", axis}});
    }

    void Silu(const fastllm::Data &input, fastllm::Data &output) {
        curExecutor->Run("Silu", {
                {"input", (Data*)&input}, {"output", &output}
        }, {}, {});
    }

    void GeluNew(const fastllm::Data &input, fastllm::Data &output) {
        curExecutor->Run("GeluNew", {
                {"input", (Data*)&input}, {"output", &output}
        }, {}, {});
    }

    void Mul(const fastllm::Data &input, float v, fastllm::Data &output) {
        curExecutor->Run("Mul", {
                {"input", (Data*)&input}, {"output", &output}
        }, {{"v", v}}, {});
    }

    void MulTo(Data &input0, const Data &input1) {
        curExecutor->Run("MulTo", {
                {"input0", &input0}, {"input1", (Data*)&input1}
        }, {}, {});
    }

    void AddTo(Data &input0, const Data &input1, float alpha) {
        curExecutor->Run("AddTo", {
                {"input0", &input0}, {"input1", (Data*)&input1}
        }, {{"alpha", alpha}}, {});
    }

    void AttentionMask(Data &input, const Data &mask, float maskValue) {
        curExecutor->Run("AttentionMask", {
                {"input", &input}, {"mask", (Data*)&mask}
        }, {{"maskValue", maskValue}}, {});
    }

    void Permute(const Data &input, const std::vector<int> &axis, Data &output) {
        Data axisData = Data(DataType::INT32PARAM, {(int)axis.size()});
        axisData.Allocate();
        for (int i = 0; i < axisData.Count(0); i++) {
            ((int32_t*)axisData.cpuData)[i] = axis[i];
        }
        curExecutor->Run("Permute", {
                {"input", (Data*)&input}, {"axis", &axisData}, {"output", (Data*)&output}
        }, {}, {});
    }

    void PermuteSelf(const Data &input, const std::vector<int> &axis) {
        Data axisData = Data(DataType::INT32PARAM, {(int)axis.size()});
        axisData.Allocate();
        for (int i = 0; i < axisData.Count(0); i++) {
            ((int32_t*)axisData.cpuData)[i] = axis[i];
        }
        curExecutor->Run("PermuteSelf", {
                {"input", (Data*)&input}, {"axis", &axisData}
        }, {}, {});
    }

    void TopK(const Data &input, Data &output, int topk) {
        curExecutor->Run("TopK", {
                {"input", (Data*)&input}, {"output", &output}
        }, {}, {{"topk", topk}});
    };

    void RotatePosition2D(Data &input, const Data &positionIds, Data &sinData, Data &cosData, int rotaryDim) {
        curExecutor->Run("RotatePosition2D", {
                {"input", &input}, {"positionIds", (Data*)&positionIds}, {"sin", &sinData}, {"cos", &cosData}
        }, {}, {{"rotaryDim", rotaryDim}});
    }

    void RepeatPenalty(Data &input, const Data &penalty) {
        curExecutor->Run("RepeatPenalty", {
                {"input", &input}, {"penalty", (Data*)&penalty}
        }, {}, {});
    }
}