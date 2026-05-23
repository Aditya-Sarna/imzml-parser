// ---------------------------------------------------------------------------
// XMLHandler.h
// Abstract base class for expat-based SAX handlers.
//
// Mirrors the role of OpenMS/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h:
//   - Owns the expat parser lifetime
//   - Dispatches element start / end / character data callbacks
//   - Provides attribute-lookup helpers used by derived handlers
// ---------------------------------------------------------------------------
#pragma once

#include <expat.h>
#include <string>
#include <stdexcept>
#include <cstring>

namespace imzml
{
namespace Internal
{

// ---------------------------------------------------------------------------
// Exception types (mirrors OpenMS ParseError / FileNotFound)
// ---------------------------------------------------------------------------
struct ParseError : std::runtime_error
{
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct FileNotFound : std::runtime_error
{
    explicit FileNotFound(const std::string& file)
        : std::runtime_error("File not found: " + file) {}
};

// ---------------------------------------------------------------------------
// XMLHandler
// Subclass this and override startElement / endElement / characters.
// ---------------------------------------------------------------------------
class XMLHandler
{
public:
    explicit XMLHandler(const std::string& filename)
        : filename_(filename), parser_(nullptr)
    {}

    virtual ~XMLHandler()
    {
        destroyParser_();
    }

    // Non-copyable (owns raw expat parser)
    XMLHandler(const XMLHandler&)            = delete;
    XMLHandler& operator=(const XMLHandler&) = delete;

    // ------------------------------------------------------------------
    // Public parse entry point - called by ImzMLFile::load()
    // ------------------------------------------------------------------
    void parse();

protected:
    // ------------------------------------------------------------------
    // Override these in derived handlers (mirror OpenMS XMLHandler API)
    // ------------------------------------------------------------------
    virtual void startElement(const std::string& tag,
                              const XML_Char**   attrs) = 0;

    virtual void endElement(const std::string& tag) = 0;

    virtual void characters(const std::string& /*chars*/) {}

    // ------------------------------------------------------------------
    // Attribute helpers - same style as OpenMS attributeAsString_ etc.
    // ------------------------------------------------------------------

    /// Required attribute - throws ParseError if missing
    static std::string attributeAsString(const XML_Char** attrs,
                                         const char*      name);

    /// Optional attribute - returns defaultVal if not present
    static std::string optionalAttributeAsString(const XML_Char** attrs,
                                                 const char*      name,
                                                 const char*      defaultVal = "");

    /// Convert attribute to int64 (required)
    static int64_t  attributeAsInt64 (const XML_Char** attrs, const char* name);

    /// Convert attribute to double (required)
    static double   attributeAsDouble(const XML_Char** attrs, const char* name);

    /// Convert optional attribute to int64; returns defaultVal if absent
    static int64_t  optionalAttributeAsInt64 (const XML_Char** attrs,
                                              const char* name,
                                              int64_t defaultVal = 0);

    const std::string& filename() const { return filename_; }

private:
    // ------------------------------------------------------------------
    // expat C-style callbacks - forward to virtual methods
    // ------------------------------------------------------------------
    static void XMLCALL onStartElement_(void* userData,
                                        const XML_Char* name,
                                        const XML_Char** atts);

    static void XMLCALL onEndElement_(void* userData,
                                      const XML_Char* name);

    static void XMLCALL onCharacters_(void* userData,
                                      const XML_Char* s,
                                      int len);

    void createParser_();
    void destroyParser_();

    std::string  filename_;
    XML_Parser   parser_;
    std::string  charBuf_;  // accumulates character data between tags
};

} // namespace Internal
} // namespace imzml
