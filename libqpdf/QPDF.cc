#include <qpdf/qpdf-config.h> // include first for large file support

#include <qpdf/QPDF_private.hh>

#include <atomic>
#include <cstring>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

#include <qpdf/BufferInputSource.hh>
#include <qpdf/FileInputSource.hh>
#include <qpdf/OffsetInputSource.hh>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFLogger.hh>
#include <qpdf/QPDFObject_private.hh>
#include <qpdf/QPDFParser.hh>
#include <qpdf/QPDF_Array.hh>
#include <qpdf/QPDF_Dictionary.hh>
#include <qpdf/QPDF_Null.hh>
#include <qpdf/QPDF_Reserved.hh>
#include <qpdf/QPDF_Stream.hh>
#include <qpdf/QPDF_Unresolved.hh>
#include <qpdf/QTC.hh>
#include <qpdf/QUtil.hh>

// This must be a fixed value. This API returns a const reference to it, and the C API relies on its
// being static as well.
std::string const QPDF::qpdf_version(QPDF_VERSION);

namespace
{
    class InvalidInputSource final: public InputSource
    {
      public:
        InvalidInputSource(std::string const& name) :
            name(name)
        {
        }
        ~InvalidInputSource() final = default;
        qpdf_offset_t
        findAndSkipNextEOL() final
        {
            throwException();
            return 0;
        }
        std::string const&
        getName() const final
        {
            return name;
        }
        qpdf_offset_t
        tell() final
        {
            throwException();
            return 0;
        }
        void
        seek(qpdf_offset_t offset, int whence) final
        {
            throwException();
        }
        void
        rewind() final
        {
            throwException();
        }
        size_t
        read(char* buffer, size_t length) final
        {
            throwException();
            return 0;
        }
        void
        unreadCh(char ch) final
        {
            throwException();
        }

      private:
        void
        throwException()
        {
            throw std::logic_error("QPDF operation attempted on a QPDF object with no input "
                                   "source. QPDF operations are invalid before processFile (or "
                                   "another process method) or after closeInputSource");
        }

        std::string const& name;
    };
} // namespace

QPDF::ForeignStreamData::ForeignStreamData(
    std::shared_ptr<EncryptionParameters> encp,
    std::shared_ptr<InputSource> file,
    QPDFObjGen const& foreign_og,
    qpdf_offset_t offset,
    size_t length,
    QPDFObjectHandle local_dict) :
    encp(encp),
    file(file),
    foreign_og(foreign_og),
    offset(offset),
    length(length),
    local_dict(local_dict)
{
}

QPDF::CopiedStreamDataProvider::CopiedStreamDataProvider(QPDF& destination_qpdf) :
    QPDFObjectHandle::StreamDataProvider(true),
    destination_qpdf(destination_qpdf)
{
}

bool
QPDF::CopiedStreamDataProvider::provideStreamData(
    QPDFObjGen const& og, Pipeline* pipeline, bool suppress_warnings, bool will_retry)
{
    std::shared_ptr<ForeignStreamData> foreign_data = foreign_stream_data[og];
    bool result = false;
    if (foreign_data.get()) {
        result = destination_qpdf.pipeForeignStreamData(
            foreign_data, pipeline, suppress_warnings, will_retry);
        QTC::TC("qpdf", "QPDF copy foreign with data", result ? 0 : 1);
    } else {
        auto foreign_stream = foreign_streams[og];
        result = foreign_stream.pipeStreamData(
            pipeline, nullptr, 0, qpdf_dl_none, suppress_warnings, will_retry);
        QTC::TC("qpdf", "QPDF copy foreign with foreign_stream", result ? 0 : 1);
    }
    return result;
}

void
QPDF::CopiedStreamDataProvider::registerForeignStream(
    QPDFObjGen const& local_og, QPDFObjectHandle foreign_stream)
{
    this->foreign_streams[local_og] = foreign_stream;
}

void
QPDF::CopiedStreamDataProvider::registerForeignStream(
    QPDFObjGen const& local_og, std::shared_ptr<ForeignStreamData> foreign_stream)
{
    this->foreign_stream_data[local_og] = foreign_stream;
}

QPDF::StringDecrypter::StringDecrypter(QPDF* qpdf, QPDFObjGen const& og) :
    qpdf(qpdf),
    og(og)
{
}

void
QPDF::StringDecrypter::decryptString(std::string& val)
{
    qpdf->decryptString(val, og);
}

std::string const&
QPDF::QPDFVersion()
{
    // The C API relies on this being a static value.
    return QPDF::qpdf_version;
}

QPDF::EncryptionParameters::EncryptionParameters() :
    encrypted(false),
    encryption_initialized(false),
    encryption_V(0),
    encryption_R(0),
    encrypt_metadata(true),
    cf_stream(e_none),
    cf_string(e_none),
    cf_file(e_none),
    user_password_matched(false),
    owner_password_matched(false)
{
}

QPDF::Members::Members(QPDF& qpdf) :
    log(QPDFLogger::defaultLogger()),
    file_sp(new InvalidInputSource(no_input_name)),
    file(file_sp.get()),
    encp(new EncryptionParameters),
    objects(qpdf, this, file)
{
}

QPDF::QPDF() :
    m(new Members(*this))
{
    m->tokenizer.allowEOF();
    // Generate a unique ID. It just has to be unique among all QPDF objects allocated throughout
    // the lifetime of this running application.
    static std::atomic<unsigned long long> unique_id{0};
    m->unique_id = unique_id.fetch_add(1ULL);
}

QPDF::~QPDF() = default;

std::shared_ptr<QPDF>
QPDF::create()
{
    return std::make_shared<QPDF>();
}

void
QPDF::processFile(char const* filename, char const* password)
{
    auto* fi = new FileInputSource(filename);
    processInputSource(std::shared_ptr<InputSource>(fi), password);
}

void
QPDF::processFile(char const* description, FILE* filep, bool close_file, char const* password)
{
    auto* fi = new FileInputSource(description, filep, close_file);
    processInputSource(std::shared_ptr<InputSource>(fi), password);
}

void
QPDF::processMemoryFile(
    char const* description, char const* buf, size_t length, char const* password)
{
    processInputSource(
        std::shared_ptr<InputSource>(
            // line-break
            new BufferInputSource(
                description, new Buffer(QUtil::unsigned_char_pointer(buf), length), true)),
        password);
}

void
QPDF::processInputSource(std::shared_ptr<InputSource> source, char const* password)
{
    m->file_sp = source;
    m->file = source.get();
    parse(password);
}

void
QPDF::closeInputSource()
{
    m->no_input_name = "closed input source";
    m->file_sp = std::shared_ptr<InputSource>(new InvalidInputSource(m->no_input_name));
    m->file = m->file_sp.get();
}

void
QPDF::setPasswordIsHexKey(bool val)
{
    m->provided_password_is_hex_key = val;
}

void
QPDF::emptyPDF()
{
    m->pdf_version = "1.3";
    m->no_input_name = "empty PDF";
    m->objects.xref_table().initialize_empty();
}

void
QPDF::registerStreamFilter(
    std::string const& filter_name, std::function<std::shared_ptr<QPDFStreamFilter>()> factory)
{
    QPDF_Stream::registerStreamFilter(filter_name, factory);
}

void
QPDF::setIgnoreXRefStreams(bool val)
{
    m->objects.xref_table().ignore_streams(val);
}

std::shared_ptr<QPDFLogger>
QPDF::getLogger()
{
    return m->log;
}

void
QPDF::setLogger(std::shared_ptr<QPDFLogger> l)
{
    m->log = l;
}

void
QPDF::setOutputStreams(std::ostream* out, std::ostream* err)
{
    setLogger(QPDFLogger::create());
    m->log->setOutputStreams(out, err);
}

void
QPDF::setSuppressWarnings(bool val)
{
    m->suppress_warnings = val;
}

void
QPDF::setMaxWarnings(size_t val)
{
    m->max_warnings = val;
}

void
QPDF::setAttemptRecovery(bool val)
{
    m->attempt_recovery = val;
    m->objects.xref_table().attempt_recovery(val);
}

void
QPDF::setImmediateCopyFrom(bool val)
{
    m->immediate_copy_from = val;
}

std::vector<QPDFExc>
QPDF::getWarnings()
{
    std::vector<QPDFExc> result = m->warnings;
    m->warnings.clear();
    return result;
}

bool
QPDF::anyWarnings() const
{
    return !m->warnings.empty();
}

size_t
QPDF::numWarnings() const
{
    return m->warnings.size();
}

bool
QPDF::validatePDFVersion(char const*& p, std::string& version)
{
    bool valid = QUtil::is_digit(*p);
    if (valid) {
        while (QUtil::is_digit(*p)) {
            version.append(1, *p++);
        }
        if ((*p == '.') && QUtil::is_digit(*(p + 1))) {
            version.append(1, *p++);
            while (QUtil::is_digit(*p)) {
                version.append(1, *p++);
            }
        } else {
            valid = false;
        }
    }
    return valid;
}

bool
QPDF::findHeader()
{
    qpdf_offset_t global_offset = m->file->tell();
    std::string line = m->file->readLine(1024);
    char const* p = line.c_str();
    if (strncmp(p, "%PDF-", 5) != 0) {
        throw std::logic_error("findHeader is not looking at %PDF-");
    }
    p += 5;
    std::string version;
    // Note: The string returned by line.c_str() is always null-terminated. The code below never
    // overruns the buffer because a null character always short-circuits further advancement.
    bool valid = validatePDFVersion(p, version);
    if (valid) {
        m->pdf_version = version;
        if (global_offset != 0) {
            // Empirical evidence strongly suggests that when there is leading material prior to the
            // PDF header, all explicit offsets in the file are such that 0 points to the beginning
            // of the header.
            QTC::TC("qpdf", "QPDF global offset");
            m->file_sp =
                std::shared_ptr<InputSource>(new OffsetInputSource(m->file_sp, global_offset));
            m->file = m->file_sp.get();
        }
    }
    return valid;
}

void
QPDF::parse(char const* password)
{
    if (password) {
        m->encp->provided_password = password;
    }

    // Find the header anywhere in the first 1024 bytes of the file.
    PatternFinder hf(*this, &QPDF::findHeader);
    if (!m->file->findFirst("%PDF-", 0, 1024, hf)) {
        QTC::TC("qpdf", "QPDF not a pdf file");
        warn(damagedPDF("", 0, "can't find PDF header"));
        // QPDFWriter writes files that usually require at least version 1.2 for /FlateDecode
        m->pdf_version = "1.2";
    }

    m->objects.xref_table().initialize();
    initializeEncryption();
    if (m->objects.xref_table().size() > 0 && !getRoot().getKey("/Pages").isDictionary()) {
        // QPDFs created from JSON have an empty xref table and no root object yet.
        throw damagedPDF("", 0, "unable to find page tree");
    }
}

void
QPDF::inParse(bool v)
{
    if (m->in_parse == v) {
        // This happens if QPDFParser::parse tries to resolve an indirect object while it is
        // parsing.
        throw std::logic_error("QPDF: re-entrant parsing detected. This is a qpdf bug."
                               " Please report at https://github.com/qpdf/qpdf/issues.");
    }
    m->in_parse = v;
}

void
QPDF::warn(QPDFExc const& e)
{
    if (m->max_warnings > 0 && m->warnings.size() >= m->max_warnings) {
        stopOnError("Too many warnings - file is too badly damaged");
    }
    m->warnings.push_back(e);
    if (!m->suppress_warnings) {
        *m->log->getWarn() << "WARNING: " << m->warnings.back().what() << "\n";
    }
}

void
QPDF::warn(
    qpdf_error_code_e error_code,
    std::string const& object,
    qpdf_offset_t offset,
    std::string const& message)
{
    warn(QPDFExc(error_code, getFilename(), object, offset, message));
}

void
QPDF::showXRefTable()
{
    m->objects.xref_table().show();
}

// Ensure all objects in the pdf file, including those in indirect references, appear in the object
// cache.
void
QPDF::fixDanglingReferences(bool force)
{
    if (m->fixed_dangling_refs) {
        return;
    }
    if (!m->objects.xref_table().resolve()) {
        QTC::TC("qpdf", "QPDF fix dangling triggered xref reconstruction");
        m->objects.xref_table().resolve();
    }
    m->fixed_dangling_refs = true;
}

size_t
QPDF::getObjectCount()
{
    // This method returns the next available indirect object number. makeIndirectObject uses it for
    // this purpose. After fixDanglingReferences is called, all objects in the xref table will also
    // be in obj_cache.
    return toS(m->objects.next_id().getObj() - 1);
}

std::vector<QPDFObjectHandle>
QPDF::getAllObjects()
{
    return m->objects.all();
}

void
QPDF::setLastObjectDescription(std::string const& description, QPDFObjGen const& og)
{
    m->last_object_description.clear();
    if (!description.empty()) {
        m->last_object_description += description;
        if (og.isIndirect()) {
            m->last_object_description += ": ";
        }
    }
    if (og.isIndirect()) {
        m->last_object_description += "object " + og.unparse(' ');
    }
}

QPDFTokenizer::Token
QPDF::readToken(InputSource& input, size_t max_len)
{
    return m->tokenizer.readToken(input, m->last_object_description, true, max_len);
}

QPDFObjectHandle
QPDF::newIndirect(QPDFObjGen const& og, std::shared_ptr<QPDFObject> const& obj)
{
    obj->setDefaultDescription(this, og);
    return {obj};
}

QPDFObjectHandle
QPDF::makeIndirectObject(QPDFObjectHandle oh)
{
    if (!oh) {
        throw std::logic_error("attempted to make an uninitialized QPDFObjectHandle indirect");
    }
    return m->objects.make_indirect(oh.getObj());
}

QPDFObjectHandle
QPDF::newReserved()
{
    return m->objects.make_indirect(QPDF_Reserved::create());
}

QPDFObjectHandle
QPDF::newIndirectNull()
{
    return m->objects.make_indirect(QPDF_Null::create());
}

QPDFObjectHandle
QPDF::newStream()
{
    return m->objects.make_indirect(
        QPDF_Stream::create(this, m->objects.next_id(), QPDFObjectHandle::newDictionary(), 0, 0));
}

QPDFObjectHandle
QPDF::newStream(std::shared_ptr<Buffer> data)
{
    auto result = newStream();
    result.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    return result;
}

QPDFObjectHandle
QPDF::newStream(std::string const& data)
{
    auto result = newStream();
    result.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    return result;
}

QPDFObjectHandle
QPDF::getObject(QPDFObjGen const& og)
{
    return m->objects.get(og);
}

QPDFObjectHandle
QPDF::getObject(int id, int gen)
{
    return m->objects.get(id, gen);
}

QPDFObjectHandle
QPDF::getObjectByObjGen(QPDFObjGen const& og)
{
    return m->objects.get(og);
}

QPDFObjectHandle
QPDF::getObjectByID(int id, int gen)
{
    return m->objects.get(id, gen);
}

void
QPDF::replaceObject(int id, int gen, QPDFObjectHandle replacement)
{
    m->objects.replace(QPDFObjGen(id, gen), replacement);
}

void
QPDF::replaceObject(QPDFObjGen const& og, QPDFObjectHandle replacement)
{
    m->objects.replace(og, replacement);
}

void
QPDF::replaceReserved(QPDFObjectHandle reserved, QPDFObjectHandle replacement)
{
    QTC::TC("qpdf", "QPDF replaceReserved");
    auto tc = reserved.getTypeCode();
    if (!(tc == ::ot_reserved || tc == ::ot_null)) {
        throw std::logic_error("replaceReserved called with non-reserved object");
    }
    m->objects.replace(reserved.getObjGen(), replacement);
}

QPDFObjectHandle
QPDF::copyForeignObject(QPDFObjectHandle foreign)
{
    // Here's an explanation of what's going on here.
    //
    // A QPDFObjectHandle that is an indirect object has an owning QPDF. The object ID and
    // generation refers to an object in the owning QPDF. When we copy the QPDFObjectHandle from a
    // foreign QPDF into the local QPDF, we have to replace all indirect object references with
    // references to the corresponding object in the local file.
    //
    // To do this, we maintain mappings from foreign object IDs to local object IDs for each foreign
    // QPDF that we are copying from. The mapping is stored in an ObjCopier, which contains a
    // mapping from the foreign ObjGen to the local QPDFObjectHandle.
    //
    // To copy, we do a deep traversal of the foreign object with loop detection to discover all
    // indirect objects that are encountered, stopping at page boundaries. Whenever we encounter an
    // indirect object, we check to see if we have already created a local copy of it. If not, we
    // allocate a "reserved" object (or, for a stream, just a new stream) and store in the map the
    // mapping from the foreign object ID to the new object. While we
    // do this, we keep a list of objects to copy.
    //
    // Once we are done with the traversal, we copy all the objects that we need to copy. However,
    // the copies will contain indirect object IDs that refer to objects in the foreign file. We
    // need to replace them with references to objects in the local file. This is what
    // replaceForeignIndirectObjects does. Once we have created a copy of the foreign object with
    // all the indirect references replaced with new ones in the local context, we can replace the
    // local reserved object with the copy. This mechanism allows us to copy objects with circular
    // references in any order.

    // For streams, rather than copying the objects, we set up the stream data to pull from the
    // original stream by using a stream data provider. This is done in a manner that doesn't
    // require the original QPDF object but may require the original source of the stream data with
    // special handling for immediate_copy_from. This logic is also in
    // replaceForeignIndirectObjects.

    // Note that we explicitly allow use of copyForeignObject on page objects. It is a documented
    // use case to copy pages this way if the intention is to not update the pages tree.
    if (!foreign.isIndirect()) {
        QTC::TC("qpdf", "QPDF copyForeign direct");
        throw std::logic_error("QPDF::copyForeign called with direct object handle");
    }
    QPDF& other = foreign.getQPDF();
    if (&other == this) {
        QTC::TC("qpdf", "QPDF copyForeign not foreign");
        throw std::logic_error("QPDF::copyForeign called with object from this QPDF");
    }

    ObjCopier& obj_copier = m->object_copiers[other.m->unique_id];
    if (!obj_copier.visiting.empty()) {
        throw std::logic_error("obj_copier.visiting is not empty"
                               " at the beginning of copyForeignObject");
    }

    // Make sure we have an object in this file for every referenced object in the old file.
    // obj_copier.object_map maps foreign QPDFObjGen to local objects.  For everything new that we
    // have to copy, the local object will be a reservation, unless it is a stream, in which case
    // the local object will already be a stream.
    reserveObjects(foreign, obj_copier, true);

    if (!obj_copier.visiting.empty()) {
        throw std::logic_error("obj_copier.visiting is not empty after reserving objects");
    }

    // Copy any new objects and replace the reservations.
    for (auto& to_copy: obj_copier.to_copy) {
        QPDFObjectHandle copy = replaceForeignIndirectObjects(to_copy, obj_copier, true);
        if (!to_copy.isStream()) {
            QPDFObjGen og(to_copy.getObjGen());
            replaceReserved(obj_copier.object_map[og], copy);
        }
    }
    obj_copier.to_copy.clear();

    auto og = foreign.getObjGen();
    if (!obj_copier.object_map.count(og)) {
        warn(damagedPDF("unexpected reference to /Pages object while copying foreign object; "
                        "replacing with null"));
        return QPDFObjectHandle::newNull();
    }
    return obj_copier.object_map[foreign.getObjGen()];
}

void
QPDF::reserveObjects(QPDFObjectHandle foreign, ObjCopier& obj_copier, bool top)
{
    auto foreign_tc = foreign.getTypeCode();
    if (foreign_tc == ::ot_reserved) {
        throw std::logic_error("QPDF: attempting to copy a foreign reserved object");
    }

    if (foreign.isPagesObject()) {
        QTC::TC("qpdf", "QPDF not copying pages object");
        return;
    }

    if (foreign.isIndirect()) {
        QPDFObjGen foreign_og(foreign.getObjGen());
        if (!obj_copier.visiting.add(foreign_og)) {
            QTC::TC("qpdf", "QPDF loop reserving objects");
            return;
        }
        if (obj_copier.object_map.count(foreign_og) > 0) {
            QTC::TC("qpdf", "QPDF already reserved object");
            if (!(top && foreign.isPageObject() && obj_copier.object_map[foreign_og].isNull())) {
                obj_copier.visiting.erase(foreign);
                return;
            }
        } else {
            QTC::TC("qpdf", "QPDF copy indirect");
            obj_copier.object_map[foreign_og] =
                foreign.isStream() ? newStream() : newIndirectNull();
            if ((!top) && foreign.isPageObject()) {
                QTC::TC("qpdf", "QPDF not crossing page boundary");
                obj_copier.visiting.erase(foreign_og);
                return;
            }
        }
        obj_copier.to_copy.push_back(foreign);
    }

    if (foreign_tc == ::ot_array) {
        QTC::TC("qpdf", "QPDF reserve array");
        int n = foreign.getArrayNItems();
        for (int i = 0; i < n; ++i) {
            reserveObjects(foreign.getArrayItem(i), obj_copier, false);
        }
    } else if (foreign_tc == ::ot_dictionary) {
        QTC::TC("qpdf", "QPDF reserve dictionary");
        for (auto const& key: foreign.getKeys()) {
            reserveObjects(foreign.getKey(key), obj_copier, false);
        }
    } else if (foreign_tc == ::ot_stream) {
        QTC::TC("qpdf", "QPDF reserve stream");
        reserveObjects(foreign.getDict(), obj_copier, false);
    }

    obj_copier.visiting.erase(foreign);
}

QPDFObjectHandle
QPDF::replaceForeignIndirectObjects(QPDFObjectHandle foreign, ObjCopier& obj_copier, bool top)
{
    auto foreign_tc = foreign.getTypeCode();
    QPDFObjectHandle result;
    if ((!top) && foreign.isIndirect()) {
        QTC::TC("qpdf", "QPDF replace indirect");
        auto mapping = obj_copier.object_map.find(foreign.getObjGen());
        if (mapping == obj_copier.object_map.end()) {
            // This case would occur if this is a reference to a Pages object that we didn't
            // traverse into.
            QTC::TC("qpdf", "QPDF replace foreign indirect with null");
            result = QPDFObjectHandle::newNull();
        } else {
            result = mapping->second;
        }
    } else if (foreign_tc == ::ot_array) {
        QTC::TC("qpdf", "QPDF replace array");
        result = QPDFObjectHandle::newArray();
        int n = foreign.getArrayNItems();
        for (int i = 0; i < n; ++i) {
            result.appendItem(
                // line-break
                replaceForeignIndirectObjects(foreign.getArrayItem(i), obj_copier, false));
        }
    } else if (foreign_tc == ::ot_dictionary) {
        QTC::TC("qpdf", "QPDF replace dictionary");
        result = QPDFObjectHandle::newDictionary();
        std::set<std::string> keys = foreign.getKeys();
        for (auto const& iter: keys) {
            result.replaceKey(
                iter, replaceForeignIndirectObjects(foreign.getKey(iter), obj_copier, false));
        }
    } else if (foreign_tc == ::ot_stream) {
        QTC::TC("qpdf", "QPDF replace stream");
        result = obj_copier.object_map[foreign.getObjGen()];
        result.assertStream();
        QPDFObjectHandle dict = result.getDict();
        QPDFObjectHandle old_dict = foreign.getDict();
        std::set<std::string> keys = old_dict.getKeys();
        for (auto const& iter: keys) {
            dict.replaceKey(
                iter, replaceForeignIndirectObjects(old_dict.getKey(iter), obj_copier, false));
        }
        copyStreamData(result, foreign);
    } else {
        foreign.assertScalar();
        result = foreign;
        result.makeDirect();
    }

    if (top && (!result.isStream()) && result.isIndirect()) {
        throw std::logic_error("replacement for foreign object is indirect");
    }

    return result;
}

void
QPDF::copyStreamData(QPDFObjectHandle result, QPDFObjectHandle foreign)
{
    // This method was originally written for copying foreign streams, but it is used by
    // QPDFObjectHandle to copy streams from the same QPDF object as well.

    QPDFObjectHandle dict = result.getDict();
    QPDFObjectHandle old_dict = foreign.getDict();
    if (m->copied_stream_data_provider == nullptr) {
        m->copied_stream_data_provider = new CopiedStreamDataProvider(*this);
        m->copied_streams =
            std::shared_ptr<QPDFObjectHandle::StreamDataProvider>(m->copied_stream_data_provider);
    }
    QPDFObjGen local_og(result.getObjGen());
    // Copy information from the foreign stream so we can pipe its data later without keeping the
    // original QPDF object around.

    QPDF& foreign_stream_qpdf =
        foreign.getQPDF("unable to retrieve owning qpdf from foreign stream");

    auto stream = foreign.getObjectPtr()->as<QPDF_Stream>();
    if (stream == nullptr) {
        throw std::logic_error("unable to retrieve underlying"
                               " stream object from foreign stream");
    }
    std::shared_ptr<Buffer> stream_buffer = stream->getStreamDataBuffer();
    if ((foreign_stream_qpdf.m->immediate_copy_from) && (stream_buffer == nullptr)) {
        // Pull the stream data into a buffer before attempting the copy operation. Do it on the
        // source stream so that if the source stream is copied multiple times, we don't have to
        // keep duplicating the memory.
        QTC::TC("qpdf", "QPDF immediate copy stream data");
        foreign.replaceStreamData(
            foreign.getRawStreamData(),
            old_dict.getKey("/Filter"),
            old_dict.getKey("/DecodeParms"));
        stream_buffer = stream->getStreamDataBuffer();
    }
    std::shared_ptr<QPDFObjectHandle::StreamDataProvider> stream_provider =
        stream->getStreamDataProvider();
    if (stream_buffer.get()) {
        QTC::TC("qpdf", "QPDF copy foreign stream with buffer");
        result.replaceStreamData(
            stream_buffer, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
    } else if (stream_provider.get()) {
        // In this case, the remote stream's QPDF must stay in scope.
        QTC::TC("qpdf", "QPDF copy foreign stream with provider");
        m->copied_stream_data_provider->registerForeignStream(local_og, foreign);
        result.replaceStreamData(
            m->copied_streams, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
    } else {
        auto foreign_stream_data = std::make_shared<ForeignStreamData>(
            foreign_stream_qpdf.m->encp,
            foreign_stream_qpdf.m->file_sp,
            foreign.getObjGen(),
            stream->getParsedOffset(),
            stream->getLength(),
            dict);
        m->copied_stream_data_provider->registerForeignStream(local_og, foreign_stream_data);
        result.replaceStreamData(
            m->copied_streams, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
    }
}

void
QPDF::swapObjects(int objid1, int generation1, int objid2, int generation2)
{
    m->objects.swap(QPDFObjGen(objid1, generation1), QPDFObjGen(objid2, generation2));
}

void
QPDF::swapObjects(QPDFObjGen const& og1, QPDFObjGen const& og2)
{
    m->objects.swap(og1, og2);
}

unsigned long long
QPDF::getUniqueId() const
{
    return m->unique_id;
}

std::string
QPDF::getFilename() const
{
    return m->file->getName();
}

PDFVersion
QPDF::getVersionAsPDFVersion()
{
    int major = 1;
    int minor = 3;
    int extension_level = getExtensionLevel();

    std::regex v("^[[:space:]]*([0-9]+)\\.([0-9]+)");
    std::smatch match;
    if (std::regex_search(m->pdf_version, match, v)) {
        major = QUtil::string_to_int(match[1].str().c_str());
        minor = QUtil::string_to_int(match[2].str().c_str());
    }

    return {major, minor, extension_level};
}

std::string
QPDF::getPDFVersion() const
{
    return m->pdf_version;
}

int
QPDF::getExtensionLevel()
{
    int result = 0;
    QPDFObjectHandle obj = getRoot();
    if (obj.hasKey("/Extensions")) {
        obj = obj.getKey("/Extensions");
        if (obj.isDictionary() && obj.hasKey("/ADBE")) {
            obj = obj.getKey("/ADBE");
            if (obj.isDictionary() && obj.hasKey("/ExtensionLevel")) {
                obj = obj.getKey("/ExtensionLevel");
                if (obj.isInteger()) {
                    result = obj.getIntValueAsInt();
                }
            }
        }
    }
    return result;
}

QPDFObjectHandle
QPDF::getTrailer()
{
    return m->objects.trailer();
}

QPDFObjectHandle
QPDF::getRoot()
{
    auto root = m->objects.trailer().getKey("/Root");
    if (!root.isDictionary()) {
        throw damagedPDF("", 0, "unable to find /Root dictionary");
    } else if (
        // Check_mode is an interim solution to request #810 pending a more comprehensive review of
        // the approach to more extensive checks and warning levels.
        m->check_mode && !root.getKey("/Type").isNameAndEquals("/Catalog")) {
        warn(damagedPDF("", 0, "catalog /Type entry missing or invalid"));
        root.replaceKey("/Type", "/Catalog"_qpdf);
    }
    return root;
}

std::map<QPDFObjGen, QPDFXRefEntry>
QPDF::getXRefTable()
{
    if (!m->objects.xref_table().initialized()) {
        throw std::logic_error("QPDF::getXRefTable called before parsing.");
    }
    return m->objects.xref_table().as_map();
}

bool
QPDF::pipeStreamData(
    std::shared_ptr<EncryptionParameters> encp,
    std::shared_ptr<InputSource> file,
    QPDF& qpdf_for_warning,
    QPDFObjGen const& og,
    qpdf_offset_t offset,
    size_t length,
    QPDFObjectHandle stream_dict,
    Pipeline* pipeline,
    bool suppress_warnings,
    bool will_retry)
{
    std::unique_ptr<Pipeline> to_delete;
    if (encp->encrypted) {
        decryptStream(encp, file, qpdf_for_warning, pipeline, og, stream_dict, to_delete);
    }

    bool attempted_finish = false;
    try {
        file->seek(offset, SEEK_SET);
        auto buf = std::make_unique<char[]>(length);
        if (auto read = file->read(buf.get(), length); read != length) {
            throw damagedPDF(*file, "", offset + toO(read), "unexpected EOF reading stream data");
        }
        pipeline->write(buf.get(), length);
        attempted_finish = true;
        pipeline->finish();
        return true;
    } catch (QPDFExc& e) {
        if (!suppress_warnings) {
            qpdf_for_warning.warn(e);
        }
    } catch (std::exception& e) {
        if (!suppress_warnings) {
            QTC::TC("qpdf", "QPDF decoding error warning");
            qpdf_for_warning.warn(
                // line-break
                damagedPDF(
                    *file,
                    "",
                    file->getLastOffset(),
                    ("error decoding stream data for object " + og.unparse(' ') + ": " +
                     e.what())));
            if (will_retry) {
                qpdf_for_warning.warn(
                    // line-break
                    damagedPDF(
                        *file,
                        "",
                        file->getLastOffset(),
                        "stream will be re-processed without filtering to avoid data loss"));
            }
        }
    }
    if (!attempted_finish) {
        try {
            pipeline->finish();
        } catch (std::exception&) {
            // ignore
        }
    }
    return false;
}

bool
QPDF::pipeStreamData(
    QPDFObjGen const& og,
    qpdf_offset_t offset,
    size_t length,
    QPDFObjectHandle stream_dict,
    Pipeline* pipeline,
    bool suppress_warnings,
    bool will_retry)
{
    return pipeStreamData(
        m->encp,
        m->file_sp,
        *this,
        og,
        offset,
        length,
        stream_dict,
        pipeline,
        suppress_warnings,
        will_retry);
}

bool
QPDF::pipeForeignStreamData(
    std::shared_ptr<ForeignStreamData> foreign,
    Pipeline* pipeline,
    bool suppress_warnings,
    bool will_retry)
{
    if (foreign->encp->encrypted) {
        QTC::TC("qpdf", "QPDF pipe foreign encrypted stream");
    }
    return pipeStreamData(
        foreign->encp,
        foreign->file,
        *this,
        foreign->foreign_og,
        foreign->offset,
        foreign->length,
        foreign->local_dict,
        pipeline,
        suppress_warnings,
        will_retry);
}

// Throw a generic exception when we lack context for something more specific. New code should not
// use this. This method exists to improve somewhat from calling assert in very old code.
void
QPDF::stopOnError(std::string const& message)
{
    throw damagedPDF("", message);
}

// Return an exception of type qpdf_e_damaged_pdf.
QPDFExc
QPDF::damagedPDF(
    InputSource& input, std::string const& object, qpdf_offset_t offset, std::string const& message)
{
    return {qpdf_e_damaged_pdf, input.getName(), object, offset, message};
}

// Return an exception of type qpdf_e_damaged_pdf.  The object is taken from
// m->last_object_description.
QPDFExc
QPDF::damagedPDF(InputSource& input, qpdf_offset_t offset, std::string const& message)
{
    return damagedPDF(input, m->last_object_description, offset, message);
}

// Return an exception of type qpdf_e_damaged_pdf.  The filename is taken from m->file.
QPDFExc
QPDF::damagedPDF(std::string const& object, qpdf_offset_t offset, std::string const& message)
{
    return {qpdf_e_damaged_pdf, m->file->getName(), object, offset, message};
}

// Return an exception of type qpdf_e_damaged_pdf.  The filename is taken from m->file and the
// offset from .m->file->getLastOffset().
QPDFExc
QPDF::damagedPDF(std::string const& object, std::string const& message)
{
    return damagedPDF(object, m->file->getLastOffset(), message);
}

// Return an exception of type qpdf_e_damaged_pdf. The filename is taken from m->file and the object
// from .m->last_object_description.
QPDFExc
QPDF::damagedPDF(qpdf_offset_t offset, std::string const& message)
{
    return damagedPDF(m->last_object_description, offset, message);
}

// Return an exception of type qpdf_e_damaged_pdf.  The filename is taken from m->file, the object
// from m->last_object_description and the offset from m->file->getLastOffset().
QPDFExc
QPDF::damagedPDF(std::string const& message)
{
    return damagedPDF(m->last_object_description, m->file->getLastOffset(), message);
}

bool
QPDF::everCalledGetAllPages() const
{
    return m->ever_called_get_all_pages;
}

bool
QPDF::everPushedInheritedAttributesToPages() const
{
    return m->ever_pushed_inherited_attributes_to_pages;
}

void
QPDF::removeSecurityRestrictions()
{
    auto root = getRoot();
    root.removeKey("/Perms");
    auto acroform = root.getKey("/AcroForm");
    if (acroform.isDictionary() && acroform.hasKey("/SigFlags")) {
        acroform.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(0));
    }
}