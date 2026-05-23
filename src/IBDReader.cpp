// ---------------------------------------------------------------------------
// IBDReader.cpp
// Binary .ibd file reader with typed decode and bounds validation.
//
// The .ibd format is a flat binary file; arrays are stored one after another.
// imzML stores byte offsets and element counts in the XML; this reader
// seeks to those offsets and interprets the raw bytes as the correct type.
//
// Supported types: float32, float64, int32, int64
// All values are promoted to double in readArray() for caller convenience.
// ---------------------------------------------------------------------------
#include "imzml/IBDReader.h"

#include <sstream>
#include <stdexcept>
#include <cstring>
#include <limits>

namespace imzml
{

// ---------------------------------------------------------------------------
// Helpers: byte-level read and typed decode
// The imzML spec says data is stored in little-endian order.  Most modern
// hardware is already little-endian; this reader handles that case directly.
// If big-endian support is needed in future, byte-swap stubs are noted.
// ---------------------------------------------------------------------------
namespace
{

// Detect host endianness at compile time
inline bool isLittleEndian()
{
    constexpr uint32_t v = 1u;
    return *reinterpret_cast<const uint8_t*>(&v) == 1;
}

// Swap bytes of a T-sized integer (needed for big-endian hosts)
template<typename T>
T byteSwap(T val)
{
    T result{};
    uint8_t* src = reinterpret_cast<uint8_t*>(&val);
    uint8_t* dst = reinterpret_cast<uint8_t*>(&result);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        dst[i] = src[sizeof(T) - 1 - i];
    return result;
}

template<typename T>
T maybeSwap(T v)
{
    // imzML is always stored little-endian
    if (isLittleEndian())
        return v;
    return byteSwap(v);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
IBDReader::IBDReader(const std::string& ibdPath)
{
    open(ibdPath);
}

IBDReader::~IBDReader()
{
    close();
}

IBDReader::IBDReader(IBDReader&& other) noexcept
    : stream_(std::move(other.stream_))
    , fileSize_(other.fileSize_)
    , path_(std::move(other.path_))
{
    other.fileSize_ = 0;
}

IBDReader& IBDReader::operator=(IBDReader&& other) noexcept
{
    if (this != &other)
    {
        close();
        stream_    = std::move(other.stream_);
        fileSize_  = other.fileSize_;
        path_      = std::move(other.path_);
        other.fileSize_ = 0;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------
void IBDReader::open(const std::string& ibdPath)
{
    path_ = ibdPath;
    stream_.open(ibdPath, std::ios::binary);
    if (!stream_.is_open())
    {
        throw std::runtime_error("Cannot open .ibd file: " + ibdPath);
    }

    // Determine file size
    stream_.seekg(0, std::ios::end);
    fileSize_ = static_cast<uint64_t>(stream_.tellg());
    stream_.seekg(0, std::ios::beg);
}

void IBDReader::close()
{
    if (stream_.is_open())
        stream_.close();
    fileSize_ = 0;
}

// ---------------------------------------------------------------------------
// validateRange
// Checks that [offset, offset + numElements * sizeof(type)) fits in file.
// ---------------------------------------------------------------------------
bool IBDReader::validateRange(uint64_t       offset,
                               uint64_t       numElements,
                               BinaryDataType dataType,
                               std::string&   errMsg) const
{
    if (!stream_.is_open())
    {
        errMsg = "IBD file is not open";
        return false;
    }
    if (dataType == BinaryDataType::Unknown)
    {
        errMsg = "Unknown data type for array at offset " + std::to_string(offset);
        return false;
    }

    std::size_t elemBytes = bytesForType(dataType);
    // Overflow check before multiply
    if (numElements > (std::numeric_limits<uint64_t>::max() / elemBytes))
    {
        errMsg = "Element count overflow: " + std::to_string(numElements) +
                 " * " + std::to_string(elemBytes);
        return false;
    }

    uint64_t totalBytes = numElements * elemBytes;
    if (offset + totalBytes > fileSize_)
    {
        std::ostringstream oss;
        oss << "Binary array out of bounds: offset=" << offset
            << " count=" << numElements
            << " totalBytes=" << totalBytes
            << " fileSize=" << fileSize_;
        errMsg = oss.str();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// decodeAs_<T> - template implementation
// Reads `length` values of type T from the stream at `offset`
// and returns them as vector<double>.
// ---------------------------------------------------------------------------
template<typename T>
std::vector<double> IBDReader::decodeAs_(uint64_t offset, uint64_t length) const
{
    std::vector<double> result;
    result.reserve(static_cast<std::size_t>(length));

    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_)
        throw std::runtime_error("seekg failed at offset " + std::to_string(offset));

    // Read all bytes in one shot
    std::vector<uint8_t> rawBytes(length * sizeof(T));
    stream_.read(reinterpret_cast<char*>(rawBytes.data()),
                 static_cast<std::streamsize>(rawBytes.size()));

    std::streamsize bytesRead = stream_.gcount();
    if (static_cast<uint64_t>(bytesRead) != rawBytes.size())
    {
        throw std::runtime_error("Unexpected end of .ibd at offset " +
                                 std::to_string(offset));
    }

    // Decode values
    const T* ptr = reinterpret_cast<const T*>(rawBytes.data());
    for (uint64_t i = 0; i < length; ++i)
    {
        T v = maybeSwap(ptr[i]);
        result.push_back(static_cast<double>(v));
    }
    return result;
}

// Explicit instantiations to keep linker happy
template std::vector<double> IBDReader::decodeAs_<float>   (uint64_t, uint64_t) const;
template std::vector<double> IBDReader::decodeAs_<double>  (uint64_t, uint64_t) const;
template std::vector<double> IBDReader::decodeAs_<int32_t> (uint64_t, uint64_t) const;
template std::vector<double> IBDReader::decodeAs_<int64_t> (uint64_t, uint64_t) const;

// ---------------------------------------------------------------------------
// readArray - public dispatch
// ---------------------------------------------------------------------------
std::vector<double> IBDReader::readArray(uint64_t       offset,
                                          uint64_t       length,
                                          BinaryDataType dataType) const
{
    // Bounds check first
    std::string errMsg;
    if (!validateRange(offset, length, dataType, errMsg))
        throw std::runtime_error(errMsg);

    switch (dataType)
    {
        case BinaryDataType::Float32: return decodeAs_<float>   (offset, length);
        case BinaryDataType::Float64: return decodeAs_<double>  (offset, length);
        case BinaryDataType::Int32:   return decodeAs_<int32_t> (offset, length);
        case BinaryDataType::Int64:   return decodeAs_<int64_t> (offset, length);
        default:
            throw std::runtime_error("readArray: unknown data type");
    }
}

// ---------------------------------------------------------------------------
// readArrayFloat32 - reads float32, returns as float (no double conversion)
// ---------------------------------------------------------------------------
std::vector<float> IBDReader::readArrayFloat32(uint64_t offset, uint64_t length) const
{
    std::string errMsg;
    if (!validateRange(offset, length, BinaryDataType::Float32, errMsg))
        throw std::runtime_error(errMsg);

    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    std::vector<float> result(static_cast<std::size_t>(length));
    stream_.read(reinterpret_cast<char*>(result.data()),
                 static_cast<std::streamsize>(length * sizeof(float)));

    if (!isLittleEndian())
    {
        for (auto& v : result)
        {
            uint32_t tmp;
            std::memcpy(&tmp, &v, 4);
            tmp = byteSwap(tmp);
            std::memcpy(&v, &tmp, 4);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// readSpectrum - convenience wrapper
// ---------------------------------------------------------------------------
SpectrumData IBDReader::readSpectrum(const ImzMLSpectrum& spec) const
{
    if (!isOpen())
        throw std::runtime_error("IBD file not open when reading spectrum index " +
                                 std::to_string(spec.index));

    SpectrumData data;

    // Validate mz array
    {
        std::string err;
        if (!validateRange(spec.mzOffset, spec.mzLength, spec.mzDataType, err))
        {
            throw std::runtime_error("Spectrum " + std::to_string(spec.index) +
                                     " m/z array: " + err);
        }
    }

    // Validate intensity array
    {
        std::string err;
        if (!validateRange(spec.intensityOffset, spec.intensityLength,
                           spec.intensityDataType, err))
        {
            throw std::runtime_error("Spectrum " + std::to_string(spec.index) +
                                     " intensity array: " + err);
        }
    }

    data.mz        = readArray(spec.mzOffset,        spec.mzLength,
                               spec.mzDataType);
    data.intensity = readArray(spec.intensityOffset,  spec.intensityLength,
                               spec.intensityDataType);

    return data;
}

} // namespace imzml
