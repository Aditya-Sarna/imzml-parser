// ---------------------------------------------------------------------------
// XMLHandler.cpp
// Base SAX handler implementation using expat.
// ---------------------------------------------------------------------------
#include "imzml/XMLHandler.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cerrno>

namespace imzml
{
namespace Internal
{

// ---------------------------------------------------------------------------
// expat callback forwarders
// ---------------------------------------------------------------------------
void XMLCALL XMLHandler::onStartElement_(void* userData,
                                         const XML_Char* name,
                                         const XML_Char** atts)
{
    auto* self = static_cast<XMLHandler*>(userData);
    // Flush any accumulated character data before we enter a new element
    if (!self->charBuf_.empty())
    {
        self->characters(self->charBuf_);
        self->charBuf_.clear();
    }
    self->startElement(std::string(name), atts);
}

void XMLCALL XMLHandler::onEndElement_(void* userData,
                                        const XML_Char* name)
{
    auto* self = static_cast<XMLHandler*>(userData);
    // Flush accumulated character data
    if (!self->charBuf_.empty())
    {
        self->characters(self->charBuf_);
        self->charBuf_.clear();
    }
    self->endElement(std::string(name));
}

void XMLCALL XMLHandler::onCharacters_(void* userData,
                                        const XML_Char* s,
                                        int len)
{
    auto* self = static_cast<XMLHandler*>(userData);
    self->charBuf_.append(s, static_cast<std::size_t>(len));
}

// ---------------------------------------------------------------------------
// Parser lifecycle
// ---------------------------------------------------------------------------
void XMLHandler::createParser_()
{
    parser_ = XML_ParserCreate(nullptr); // nullptr => use internal default encoding
    if (!parser_)
        throw ParseError("Failed to create expat XML parser");

    XML_SetUserData(parser_, this);
    XML_SetElementHandler(parser_, onStartElement_, onEndElement_);
    XML_SetCharacterDataHandler(parser_, onCharacters_);
}

void XMLHandler::destroyParser_()
{
    if (parser_)
    {
        XML_ParserFree(parser_);
        parser_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// parse() - reads the file and drives expat
// ---------------------------------------------------------------------------
void XMLHandler::parse()
{
    // Verify the file exists before creating the parser
    std::ifstream file(filename_, std::ios::binary);
    if (!file.is_open())
        throw FileNotFound(filename_);

    createParser_();

    constexpr std::size_t BUFFER_SIZE = 65536; // 64 KB read chunks
    char buf[BUFFER_SIZE];

    while (file.good())
    {
        file.read(buf, BUFFER_SIZE);
        std::streamsize bytesRead = file.gcount();

        int isFinal = file.eof() ? 1 : 0;
        if (XML_Parse(parser_, buf, static_cast<int>(bytesRead), isFinal) == XML_STATUS_ERROR)
        {
            std::ostringstream oss;
            oss << "XML parse error in '" << filename_ << "' at line "
                << XML_GetCurrentLineNumber(parser_) << ": "
                << XML_ErrorString(XML_GetErrorCode(parser_));
            destroyParser_();
            throw ParseError(oss.str());
        }

        if (isFinal)
            break;
    }

    destroyParser_();
}

// ---------------------------------------------------------------------------
// Attribute helpers
// ---------------------------------------------------------------------------
std::string XMLHandler::attributeAsString(const XML_Char** attrs,
                                           const char*      name)
{
    for (int i = 0; attrs[i]; i += 2)
    {
        if (std::strcmp(attrs[i], name) == 0)
            return std::string(attrs[i + 1]);
    }
    std::ostringstream oss;
    oss << "Required XML attribute '" << name << "' not found";
    throw ParseError(oss.str());
}

std::string XMLHandler::optionalAttributeAsString(const XML_Char** attrs,
                                                   const char*      name,
                                                   const char*      defaultVal)
{
    for (int i = 0; attrs[i]; i += 2)
    {
        if (std::strcmp(attrs[i], name) == 0)
            return std::string(attrs[i + 1]);
    }
    return defaultVal ? std::string(defaultVal) : std::string{};
}

int64_t XMLHandler::attributeAsInt64(const XML_Char** attrs,
                                      const char*      name)
{
    std::string val = attributeAsString(attrs, name);
    try
    {
        return static_cast<int64_t>(std::stoll(val));
    }
    catch (const std::exception& e)
    {
        throw ParseError(std::string("Cannot convert attribute '") + name +
                         "' value '" + val + "' to integer: " + e.what());
    }
}

double XMLHandler::attributeAsDouble(const XML_Char** attrs,
                                      const char*      name)
{
    std::string val = attributeAsString(attrs, name);
    try
    {
        return std::stod(val);
    }
    catch (const std::exception& e)
    {
        throw ParseError(std::string("Cannot convert attribute '") + name +
                         "' value '" + val + "' to double: " + e.what());
    }
}

int64_t XMLHandler::optionalAttributeAsInt64(const XML_Char** attrs,
                                              const char*      name,
                                              int64_t          defaultVal)
{
    std::string val = optionalAttributeAsString(attrs, name, "");
    if (val.empty())
        return defaultVal;
    try
    {
        return static_cast<int64_t>(std::stoll(val));
    }
    catch (...)
    {
        return defaultVal;
    }
}

} // namespace Internal
} // namespace imzml
