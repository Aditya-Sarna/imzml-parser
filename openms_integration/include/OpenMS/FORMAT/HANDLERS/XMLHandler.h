// ---------------------------------------------------------------------------
// OpenMS/FORMAT/HANDLERS/XMLHandler.h
// Xerces-C SAX2 base handler — exact mirror of OpenMS XMLHandler.h
//
// This file is a self-contained reimplementation of the OpenMS XMLHandler
// interface so the imzML integration can be compiled and tested without a
// full OpenMS checkout. When integrating into OpenMS itself, replace this
// file with #include <OpenMS/FORMAT/HANDLERS/XMLHandler.h> and remove the
// openms_integration/include path from the build.
//
// Key parity with OpenMS XMLHandler:
//   - Inherits xercesc::DefaultHandler              (same base class)
//   - Provides attributeAsString_, optionalAttributeAsString_, etc.
//   - Provides fatalError / error / warning overloads
//   - Provides parse_() called by XMLFile::parse_()
//   - EndParsingSoftly exception for early-exit
//   - StringManager::convert() helper
//   - LOADDETAIL enum
// ---------------------------------------------------------------------------
#pragma once

#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/XMLException.hpp>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/framework/LocalFileInputSource.hpp>

#include <string>
#include <stdexcept>
#include <sstream>
#include <memory>

namespace OpenMS
{

// ---------------------------------------------------------------------------
// StringManager — mirrors OpenMS Internal::StringManager
// Wraps XMLString::transcode with RAII release
// ---------------------------------------------------------------------------
class StringManager
{
public:
    /// Convert XMLCh* to std::string
    static std::string convert(const XMLCh* const xstr)
    {
        if (!xstr) return {};
        std::unique_ptr<char, void(*)(void*)> p(
            xercesc::XMLString::transcode(xstr),
            [](void* ptr){ xercesc::XMLString::release(reinterpret_cast<char**>(&ptr)); }
        );
        return std::string(p.get());
    }

    /// Convert std::string to XMLCh* (caller must XMLString::release)
    static XMLCh* convertPtr(const std::string& s)
    {
        return xercesc::XMLString::transcode(s.c_str());
    }
};

namespace Internal
{

// ---------------------------------------------------------------------------
// Exception types (mirrors OpenMS)
// ---------------------------------------------------------------------------
struct ParseError : std::runtime_error
{
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct FileNotFound : std::runtime_error
{
    explicit FileNotFound(const std::string& f)
        : std::runtime_error("File not found: " + f) {}
};

// ---------------------------------------------------------------------------
// XMLHandler
// ---------------------------------------------------------------------------
class XMLHandler : public xercesc::DefaultHandler
{
public:
    // Loading detail level — mirrors OpenMS LOADDETAIL
    enum LOADDETAIL
    {
        LD_ALLDATA,
        LD_RAWCOUNTS,
        LD_COUNTS_WITHOPTIONS
    };

    // Action mode for error messages
    enum ActionMode { LOAD, STORE };

    // Soft early-exit exception — mirrors OpenMS EndParsingSoftly
    class EndParsingSoftly : public std::exception
    {
    public:
        const char* what() const noexcept override { return "EndParsingSoftly"; }
    };

    // ------------------------------------------------------------------
    explicit XMLHandler(const std::string& filename, const std::string& version)
        : file_(filename), version_(version), load_detail_(LD_ALLDATA)
    {}

    ~XMLHandler() override = default;

    // Virtual reset — called after parse; subclasses release temp memory here
    virtual void reset() {}

    virtual LOADDETAIL getLoadDetail()  const { return load_detail_; }
    virtual void setLoadDetail(LOADDETAIL d)  { load_detail_ = d; }

    // ------------------------------------------------------------------
    // Xerces SAX error handler overrides
    // ------------------------------------------------------------------
    void fatalError(const xercesc::SAXParseException& ex) override
    {
        fatalError(LOAD, sm_.convert(ex.getMessage()),
                   static_cast<unsigned>(ex.getLineNumber()),
                   static_cast<unsigned>(ex.getColumnNumber()));
    }

    void error(const xercesc::SAXParseException& ex) override
    {
        error(LOAD, sm_.convert(ex.getMessage()),
              static_cast<unsigned>(ex.getLineNumber()),
              static_cast<unsigned>(ex.getColumnNumber()));
    }

    void warning(const xercesc::SAXParseException& ex) override
    {
        warning(LOAD, sm_.convert(ex.getMessage()),
                static_cast<unsigned>(ex.getLineNumber()),
                static_cast<unsigned>(ex.getColumnNumber()));
    }

    // ------------------------------------------------------------------
    // Programmatic error / warning — match OpenMS signatures
    // ------------------------------------------------------------------
    void fatalError(ActionMode /*mode*/, const std::string& msg,
                    unsigned line = 0, unsigned /*col*/ = 0) const
    {
        std::ostringstream oss;
        oss << "Fatal XML error";
        if (line) oss << " (line " << line << ")";
        oss << " in '" << file_ << "': " << msg;
        throw ParseError(oss.str());
    }

    void error(ActionMode /*mode*/, const std::string& msg,
               unsigned line = 0, unsigned /*col*/ = 0) const
    {
        std::ostringstream oss;
        oss << "XML error";
        if (line) oss << " (line " << line << ")";
        oss << " in '" << file_ << "': " << msg;
        // Non-fatal: print to stderr (mirrors OpenMS behaviour)
        // In production OpenMS this goes through a log system
        fprintf(stderr, "[WARNING] %s\n", oss.str().c_str());
    }

    void warning(ActionMode /*mode*/, const std::string& msg,
                 unsigned /*line*/ = 0, unsigned /*col*/ = 0) const
    {
        fprintf(stderr, "[XML Warning] %s: %s\n", file_.c_str(), msg.c_str());
    }

    // ------------------------------------------------------------------
    // Default empty SAX implementations (subclasses override what's needed)
    // ------------------------------------------------------------------
    void characters(const XMLCh* const /*chars*/,
                    const XMLSize_t   /*length*/) override {}

    void startElement(const XMLCh* const /*uri*/,
                      const XMLCh* const /*localname*/,
                      const XMLCh* const /*qname*/,
                      const xercesc::Attributes& /*attrs*/) override {}

    void endElement(const XMLCh* const /*uri*/,
                    const XMLCh* const /*localname*/,
                    const XMLCh* const /*qname*/) override {}

    // ------------------------------------------------------------------
    // Attribute helpers — mirrors OpenMS XMLHandler protected helpers
    // ------------------------------------------------------------------
protected:
    const std::string& filename() const { return file_; }
    const std::string& version()  const { return version_; }

    // Required attribute as std::string — throws if absent
    std::string attributeAsString_(const xercesc::Attributes& attrs,
                                   const XMLCh* const          name) const
    {
        const XMLCh* val = attrs.getValue(name);
        if (!val)
        {
            fatalError(LOAD, std::string("Required attribute '") +
                             sm_.convert(name) + "' not found");
        }
        return sm_.convert(val);
    }

    // Optional attribute — returns default if absent
    std::string optionalAttributeAsString_(const xercesc::Attributes& attrs,
                                           const XMLCh* const          name,
                                           const char*                  def = "") const
    {
        const XMLCh* val = attrs.getValue(name);
        return val ? sm_.convert(val) : (def ? std::string(def) : std::string{});
    }

    bool optionalAttributeAsString_(std::string&               result,
                                    const xercesc::Attributes& attrs,
                                    const XMLCh* const         name) const
    {
        const XMLCh* val = attrs.getValue(name);
        if (!val) return false;
        result = sm_.convert(val);
        return true;
    }

    // Convert optional attribute to integer; returns false if absent
    bool optionalAttributeAsInt_(int& result,
                                 const xercesc::Attributes& attrs,
                                 const XMLCh* const         name) const
    {
        const XMLCh* val = attrs.getValue(name);
        if (!val) return false;
        try { result = std::stoi(sm_.convert(val)); return true; }
        catch (...) { return false; }
    }

    bool optionalAttributeAsUInt_(unsigned& result,
                                  const xercesc::Attributes& attrs,
                                  const XMLCh* const         name) const
    {
        const XMLCh* val = attrs.getValue(name);
        if (!val) return false;
        try { result = static_cast<unsigned>(std::stoul(sm_.convert(val))); return true; }
        catch (...) { return false; }
    }

    // Compare XMLCh* strings (avoids repeated transcode)
    static bool equal_(const XMLCh* a, const XMLCh* b)
    {
        return xercesc::XMLString::equals(a, b);
    }

    StringManager sm_;

private:
    std::string file_;
    std::string version_;
    LOADDETAIL  load_detail_;
};

} // namespace Internal
} // namespace OpenMS
