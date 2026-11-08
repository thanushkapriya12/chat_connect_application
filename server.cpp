#include "socket.hpp"
#include "web/static_assets.hpp"
#include <libpq-fe.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <array>
#include <system_error>
#include <limits>
#include <iomanip>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

using std::string;
using Fields = std::map<string,string>;
struct Problem { int status; string message; };
long long now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
string json(const string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char ch:value) {
        if(ch=='"'||ch=='\\')out << '\\' << ch;
        else if(ch<32)out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << int(ch);
        else out << ch;
    }
    out << '"'; return out.str();
}
string hex(const unsigned char* bytes,size_t count) {
    const char digits[]="0123456789abcdef";
    string output; output.reserve(count*2);
    for(size_t i=0;i<count;++i) {output+=digits[bytes[i]>>4];output+=digits[bytes[i]&15];}
    return output;
}
string secureRandom(size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if(RAND_bytes(data.data(),static_cast<int>(data.size()))!=1)throw Problem{500,"Security initialization failed"};
    return hex(data.data(),data.size());
}
string digest(const string& input) {
    unsigned char sum[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),input.size(),sum);
    return hex(sum,sizeof(sum));
}
string passwordHash(const string& password,const string& salt) {
    unsigned char output[32];
    if(PKCS5_PBKDF2_HMAC(password.c_str(),static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()),static_cast<int>(salt.size()),
        210000,EVP_sha256(),sizeof(output),output)!=1)throw Problem{500,"Password protection failed"};
    return hex(output,sizeof(output));
}
// Constant-time comparison for password verification.
bool constantEqual(const string& a,const string& b) {
    unsigned diff=static_cast<unsigned>(a.size()^b.size());
    for(size_t i=0;i<std::min(a.size(),b.size());++i)diff|=static_cast<unsigned char>(a[i]^b[i]);
    return diff==0;
}
bool validUsername(const string& value) {
    if(value.size()<3||value.size()>24)return false;
    return std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';});
}
bool validEmail(const string& email) {
    if(email.size()<5||email.size()>254||email.find_first_of(" \t\r\n")!=string::npos)return false;
    const auto at=email.find('@');
    return at!=string::npos&&at>0&&at==email.rfind('@')&&at+3<email.size()&&email.find('.',at+2)!=string::npos && email.back()!='.';
}
bool validPassword(const string& value) {return value.size()>=10&&value.size()<=128;}
// Profile fields use explicit byte limits because requests are UTF-8 form data.
// These values are treated as plain text throughout the frontend.
bool validProfileText(const string& input,size_t minBytes,size_t maxBytes,bool multiline) {
    if(input.size()<minBytes||input.size()>maxBytes)return false;
    if(minBytes && input.find_first_not_of(" \t\r\n")==string::npos)return false;
    for(unsigned char ch:input) {
        if(ch==0 || ch==127)return false;
        if(ch<32 && !(multiline&&(ch=='\n'||ch=='\r'||ch=='\t')))return false;
    }
    return true;
}
// Preinstalled wallpaper IDs only; never trust arbitrary URLs or filesystem paths.
bool validChatWallpaper(const string& wallpaper) {
    static const std::set<string> allowed={
        "", "doodle-blue", "kawaii-sunshine", "mint-blossom", "cat-doodle",
        "neon-love", "candy-clouds", "tropical-escape", "kitty-companions",
        "floating-hearts", "fluffy-bears", "pink-shoreline"
    };
    return allowed.count(wallpaper)!=0;
}
bool validAvatarTheme(const string& theme) {
    return theme=="cobalt"||theme=="sunset"||theme=="mint"||theme=="violet"||theme=="rose";
}
bool validPublicId(const string& input) {
    if(input.size()!=19 || input.substr(0,3)!="CC-")return false;
    return std::all_of(input.begin()+3,input.end(),[](unsigned char ch){return (ch>='0'&&ch<='9')||(ch>='A'&&ch<='F');});
}
// Untrusted client-generated request key: 128-bit random hex; never a SQL ID.
// Scoped per sender by the DB unique index, not by a client-supplied user ID.
bool validClientMessageRef(const string& id) {
    if(id.size()!=32)return false;
    return std::all_of(id.begin(),id.end(),[](unsigned char ch){
        return (ch>='0'&&ch<='9')||(ch>='a'&&ch<='f');
    });
}
bool validGroupId(const string& id) {
    return id.size()==24 && std::all_of(id.begin(),id.end(),[](unsigned char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}
bool validReactionEmoji(const string& emoji) {
    return emoji=="👍" || emoji=="❤️" || emoji=="😂" || emoji=="😮" || emoji=="😢" || emoji=="🔥";
}
bool validSqlNumber(const string& raw) {
    return !raw.empty() && raw.size()<=18 && raw[0]!='0' &&
        raw.find_first_not_of("0123456789")==string::npos;
}
string normalizedPublicId(string id) {
    for(char& ch:id)ch=char(std::toupper((unsigned char)ch));
    if(!validPublicId(id))throw Problem{400,"Enter a valid ChatConnect ID (CC- followed by 16 letters/numbers)."};
    return id;
}
string publicIdFromRandom() {
    auto id=secureRandom(8);
    for(char& ch:id)ch=char(std::toupper((unsigned char)ch));
    return "CC-"+id;
}
// Store only a small JPEG made by the browser's downscaling canvas.
// Reject SVG, HTML, oversized images, invalid Base64 and non-JPEG payloads.
bool validGroupPhoto(const string& input) {
    const string prefix="data:image/jpeg;base64,";
    if(input.rfind(prefix,0)!=0)return false;
    const string encoded=input.substr(prefix.size());
    if(encoded.empty()||encoded.size()>62000||encoded.size()%4)return false;
    size_t padding=0;
    if(encoded.back()=='=')++padding;
    if(encoded.size()>1&&encoded[encoded.size()-2]=='=')++padding;
    for(size_t i=0;i<encoded.size();++i){
        unsigned char c=static_cast<unsigned char>(encoded[i]);
        if(i>=encoded.size()-padding){if(c!='=')return false;}
        else if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'))return false;
    }
    std::vector<unsigned char> decoded(encoded.size()/4*3);
    int n=EVP_DecodeBlock(decoded.data(),
        reinterpret_cast<const unsigned char*>(encoded.data()),static_cast<int>(encoded.size()));
    if(n<0)return false;
    n-=static_cast<int>(padding);
    return n>=100&&n<=45000 && decoded[0]==0xFF&&decoded[1]==0xD8&&decoded[2]==0xFF &&
           decoded[n-2]==0xFF&&decoded[n-1]==0xD9;
}
// Attachment names are plain text, never used as filesystem paths or HTML.
// The server infers MIME from an allowlisted extension AND checks basic signatures.
string attachmentMime(const string& filename, const std::vector<unsigned char>& bytes) {
    if(filename.empty()||filename.size()>120 || filename[0]=='.')return "";
    for(unsigned char ch:filename) {
        if(ch<32 || ch==127 || ch=='/' || ch=='\\' || ch==':' || ch=='<' || ch=='>')return "";
    }
    const auto dot=filename.find_last_of('.');
    if(dot==string::npos || dot==filename.size()-1)return "";
    string ext=filename.substr(dot);
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    auto prefix=[&](std::initializer_list<unsigned char> seq){
        return bytes.size()>=seq.size() && std::equal(seq.begin(),seq.end(),bytes.begin());
    };
    if((ext==".jpg"||ext==".jpeg") && bytes.size()>=16 && prefix({0xff,0xd8,0xff}) &&
       bytes[bytes.size()-2]==0xff && bytes.back()==0xd9)return "image/jpeg";
    if(ext==".png" && prefix({0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a}))return "image/png";
    if(ext==".webp" && bytes.size()>=16 && prefix({'R','I','F','F'}) &&
       string((const char*)bytes.data()+8,4)=="WEBP")return "image/webp";
    if(ext==".pdf" && prefix({'%','P','D','F','-'}))return "application/pdf";
    // Audio uses the same 300 MiB protected chunk-upload storage as other attachments.
    // Container signatures prevent serving executable / HTML disguised as audio.
    if(ext==".webm" && prefix({0x1a,0x45,0xdf,0xa3}))return "audio/webm";
    if(ext==".ogg" && prefix({'O','g','g','S'}))return "audio/ogg";
    if(ext==".m4a" && bytes.size()>=12 &&
       bytes[4]=='f' && bytes[5]=='t' && bytes[6]=='y' && bytes[7]=='p')return "audio/mp4";
    // Modern Office documents are ZIP packages. We do NOT unpack or execute them.
    if(prefix({'P','K',3,4})){
        if(ext==".docx")return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
        if(ext==".xlsx")return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
        if(ext==".pptx")return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    }
    if(ext==".txt" && std::find(bytes.begin(),bytes.end(),0)==bytes.end())return "text/plain";
    return "";
}
constexpr std::uint64_t CC_FILE_LIMIT = 300ULL*1024*1024;
constexpr size_t CC_CHUNK_LIMIT = 512*1024;
constexpr std::uint64_t CC_VOICE_LIMIT = CC_FILE_LIMIT; // size-only limit, no recording-time cap
const std::filesystem::path CC_MEDIA_DIR = "/app/uploads";
bool validUploadId(const string& id) {
    return id.size()==32 && std::all_of(id.begin(),id.end(),[](unsigned char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
string allowedAttachmentExtension(const string& filename) {
    if(filename.empty()||filename.size()>120 || filename[0]=='.')return "";
    for(unsigned char c:filename)if(c<32||c==127||c=='/'||c=='\\'||c==':'||c=='<'||c=='>')return "";
    auto dot=filename.find_last_of('.');if(dot==string::npos)return "";
    string ext=filename.substr(dot);
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    for(const char* allow:{".jpg",".jpeg",".png",".webp",".pdf",".txt",".docx",".xlsx",".pptx",".webm",".ogg",".m4a"})
        if(ext==allow)return ext;
    return "";
}
std::uint64_t parseFileSize(const string& input) {
    if(input.empty()||input.size()>12 || input[0]=='0' || input.find_first_not_of("0123456789")!=string::npos)
        throw Problem{400,"Invalid file size."};
    std::uint64_t n=0;
    try{n=std::stoull(input);}catch(...){throw Problem{400,"Invalid file size."};}
    if(n<1||n>CC_FILE_LIMIT)throw Problem{413,"Files must be at most 300 MB."};
    return n;
}
std::uint64_t parseUploadOffset(const string& input) {
    if(input.empty()||input.size()>12||input.find_first_not_of("0123456789")!=string::npos)
        throw Problem{400,"Invalid chunk offset."};
    try{return std::stoull(input);}catch(...){throw Problem{400,"Invalid chunk offset."};}
}
std::vector<unsigned char> decodeAttachmentChunk(const string& encoded) {
    if(encoded.empty()||encoded.size()%4 || encoded.size()>((CC_CHUNK_LIMIT+2)/3)*4)
        throw Problem{413,"Chunk exceeds 512 KiB."};
    size_t pad=encoded.back()=='=' ? 1:0;
    if(encoded.size()>1&&encoded[encoded.size()-2]=='=')++pad;
    for(size_t i=0;i<encoded.size();++i){
        unsigned char c=encoded[i];
        if(i>=encoded.size()-pad){if(c!='=')throw Problem{400,"Invalid Base64 chunk."};}
        else if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'))
            throw Problem{400,"Invalid Base64 chunk."};
    }
    std::vector<unsigned char> bytes(encoded.size()/4*3);
    int n=EVP_DecodeBlock(bytes.data(),(const unsigned char*)encoded.data(),int(encoded.size()));
    if(n<=0||n<int(pad)||size_t(n-pad)>CC_CHUNK_LIMIT)throw Problem{400,"Invalid Base64 chunk."};
    bytes.resize(n-pad);
    std::vector<unsigned char> encodedAgain(((bytes.size()+2)/3)*4+4);
    const int count=EVP_EncodeBlock(encodedAgain.data(),bytes.data(),int(bytes.size()));
    if(count!=int(encoded.size()) || string((const char*)encodedAgain.data(),size_t(count))!=encoded)
        throw Problem{400,"Invalid Base64 padding bits."};
    return bytes;
}
string sniffAttachmentFile(const string& name,const std::filesystem::path& path,std::uint64_t expected) {
    std::ifstream file(path,std::ios::binary);
    if(!file)throw Problem{503,"Uploaded file is unavailable."};
    std::vector<unsigned char> prefix(16);
    file.read((char*)prefix.data(),16);
    prefix.resize(size_t(file.gcount()));
    if(expected>prefix.size()){
        file.clear();file.seekg(-2,std::ios::end);
        char tail[2];file.read(tail,2);
        if(file.gcount()!=2)throw Problem{400,"Invalid file end."};
        prefix.push_back((unsigned char)tail[0]); prefix.push_back((unsigned char)tail[1]);
    }
    const string mime=attachmentMime(name,prefix);
    if(mime=="text/plain"){
        file.clear();file.seekg(0,std::ios::beg);
        std::array<char,65536> buf{};
        while(file){file.read(buf.data(),buf.size());
            for(std::streamsize i=0;i<file.gcount();++i)if(buf[size_t(i)]=='\0')return "";
        }
    }
    return mime;
}
string decode(const string& input) {
    string value;
    for(size_t i=0;i<input.size();++i) {
        if(input[i]=='+')value+=' ';
        else if(input[i]=='%') {
            if(i+2>=input.size()||!std::isxdigit((unsigned char)input[i+1])||!std::isxdigit((unsigned char)input[i+2]))throw Problem{400,"Invalid request encoding"};
            value+=char(std::stoi(input.substr(i+1,2),nullptr,16)); i+=2;
        } else value+=input[i];
    }
    return value;
}
Fields fields(const string& body) {
    std::istringstream input(body);string pair;Fields result;
    while(std::getline(input,pair,'&')) { auto sep=pair.find('='); if(sep!=string::npos)result[decode(pair.substr(0,sep))]=decode(pair.substr(sep+1)); }
    return result;
}
struct Request { string method,path,body; Fields headers; };
Request readRequest(Socket& socket) {
    string raw;char buf[4096];
    auto readMore=[&]{int n=socket.read(buf,sizeof(buf));if(n<=0)throw Problem{400,"Incomplete request"};raw.append(buf,n);};
    while(raw.find("\r\n\r\n")==string::npos){if(raw.size()>16384)throw Problem{400,"Headers too large"};readMore();}
    size_t start=raw.find("\r\n\r\n")+4;
    std::istringstream lines(raw.substr(0,start));string line,name,value;Request r;
    std::getline(lines,line);std::istringstream requestLine(line);requestLine>>r.method>>r.path;
    while(std::getline(lines,line)&&line!="\r") {
        auto sep=line.find(':');if(sep==string::npos)throw Problem{400,"Invalid header"};
        name=line.substr(0,sep);for(char& c:name)c=char(std::tolower((unsigned char)c));
        value=line.substr(sep+1);if(!value.empty()&&value.back()=='\r')value.pop_back();
        value.erase(0,value.find_first_not_of(" \t"));r.headers[name]=value;
    }
    size_t length=0;
    if(r.headers.count("content-length")) {
        const auto v=r.headers["content-length"];
        if(v.empty()||v.size()>8||v.find_first_not_of("0123456789")!=string::npos)throw Problem{400,"Invalid length"};
        length=std::stoul(v);
    }
    // Larger POST bodies are permitted only for a small compressed JPEG group avatar.
    const size_t bodyLimit = (r.method=="POST" && r.path=="/api/attachments/chunk") ? 740000 :
        (r.method=="POST" && r.path=="/api/groups/photo") ? 90000 :
        (r.method=="POST" && r.path=="/api/calls/signal") ? 90000 : 12000;
    if(length>bodyLimit)throw Problem{413,"Request too large"};
    if(r.headers.count("transfer-encoding"))throw Problem{400,"Chunked transfer requests are not supported."};
    while(raw.size()-start<length)readMore();
    r.body=raw.substr(start,length);
    return r;
}

// Small, thread-safe per-process throttle for unauthenticated entry points.
// Uses the TCP peer rather than client-controlled X-Forwarded-For headers.
// Limits reset on process restart; production multi-instance deployments also need
// an edge/proxy or shared-store limiter.
class EntryRateLimiter {
public:
    using Clock = std::chrono::steady_clock;
    using Moment = Clock::time_point;
private:
    std::mutex guard;
    std::map<string, std::deque<Moment>> attempts;
    unsigned long long operations=0;
    static constexpr size_t MAX_BUCKETS=8192;
public:
    void enforce(const string& key, size_t maxRequests,
                 std::chrono::seconds window, Moment at=Clock::now()) {
        std::lock_guard<std::mutex> lock(guard);
        // Release old identities periodically and cap memory use under attack.
        if((++operations % 128)==0) {
            for(auto it=attempts.begin();it!=attempts.end();) {
                auto& ticks=it->second;
                // All configured windows are <= 15 minutes.
                while(!ticks.empty() && ticks.front()+std::chrono::minutes(15)<=at)
                    ticks.pop_front();
                if(ticks.empty()) it=attempts.erase(it);
                else ++it;
            }
        }
        auto existing=attempts.find(key);
        if(existing==attempts.end() && attempts.size()>=MAX_BUCKETS)
            throw Problem{429,"Too many requests. Try again later."};
        auto& ticks=attempts[key];
        while(!ticks.empty() && ticks.front()+window<=at)ticks.pop_front();
        if(ticks.size()>=maxRequests)
            throw Problem{429,"Too many requests. Try again later."};
        ticks.push_back(at);
    }
};

// Each request gets an independent PostgreSQL connection, avoiding shared libpq state across threads.
class Database {
    struct ConnectionDeleter {void operator()(PGconn* p)const {if(p)PQfinish(p);}};
    struct ResultDeleter {void operator()(PGresult* p)const {if(p)PQclear(p);}};
    std::unique_ptr<PGconn,ConnectionDeleter> connection;
public:
    using Result=std::unique_ptr<PGresult,ResultDeleter>;
    Database():connection(PQconnectdb("connect_timeout=5")) {
        if(!connection||PQstatus(connection.get())!=CONNECTION_OK) {
            std::cerr << "PostgreSQL unavailable: " << (connection?PQerrorMessage(connection.get()):"Out of memory") << '\n';
            throw Problem{503,"Database unavailable"};
        }
    }
    Result query(const string& sql,const std::vector<string>& params={}) {
        std::vector<const char*> values;
        for(const auto& p:params)values.push_back(p.c_str());
        auto result=Result(PQexecParams(connection.get(),sql.c_str(),static_cast<int>(values.size()),nullptr,values.data(),nullptr,nullptr,0));
        if(!result)throw Problem{503,"Database query failed"};
        auto status=PQresultStatus(result.get());
        if(status!=PGRES_COMMAND_OK&&status!=PGRES_TUPLES_OK) {
            const auto code=PQresultErrorField(result.get(),PG_DIAG_SQLSTATE);
            if(code&&string(code)=="23505")throw Problem{409,"That username, email, or friend request is already in use"};
            if(code&&string(code)=="23503")throw Problem{404,"That person does not exist"};
            std::cerr << "PostgreSQL query failed: " << PQresultErrorMessage(result.get()) << '\n';
            throw Problem{503,"Database operation failed"};
        }
        return result;
    }
    void executeBatch(const string& sql) {
        auto result=Result(PQexec(connection.get(),sql.c_str()));
        if(!result || PQresultStatus(result.get())!=PGRES_COMMAND_OK) {
            std::cerr << "Database schema initialization failed: " << PQerrorMessage(connection.get()) << '\n';
            throw Problem{503,"Cannot initialize database schema"};
        }
    }
    static string field(PGresult* result,int row,int col) {return PQgetisnull(result,row,col)?"":PQgetvalue(result,row,col);}
};
class Storage {
public:
    void initialize() {
        Database db;
        db.executeBatch(R"SQL(
          CREATE TABLE IF NOT EXISTS users (
            id VARCHAR(32) PRIMARY KEY,
            username VARCHAR(24) NOT NULL,
            email VARCHAR(254) NOT NULL,
            salt VARCHAR(32) NOT NULL,
            password_hash VARCHAR(64) NOT NULL,
            failed_attempts INT NOT NULL DEFAULT 0,
            locked_until TIMESTAMPTZ,
            last_seen TIMESTAMPTZ,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
          );
          CREATE UNIQUE INDEX IF NOT EXISTS users_username_unique ON users(lower(username));
          CREATE UNIQUE INDEX IF NOT EXISTS users_email_unique ON users(lower(email));
          -- Existing user data and immutable username/public ID are preserved.
          ALTER TABLE users ADD COLUMN IF NOT EXISTS display_name VARCHAR(48) NOT NULL DEFAULT '';
          ALTER TABLE users ADD COLUMN IF NOT EXISTS bio VARCHAR(160) NOT NULL DEFAULT '';
          ALTER TABLE users ADD COLUMN IF NOT EXISTS avatar_theme VARCHAR(12) NOT NULL DEFAULT 'cobalt';
          -- Preserve existing accounts, assigning each account its own permanent public ID.
          ALTER TABLE users ADD COLUMN IF NOT EXISTS public_id VARCHAR(19);
          UPDATE users SET public_id='CC-' || upper(substr(replace(gen_random_uuid()::text,'-',''),1,16))
            WHERE public_id IS NULL;
          ALTER TABLE users ALTER COLUMN public_id SET NOT NULL;
          CREATE UNIQUE INDEX IF NOT EXISTS users_public_id_unique ON users(public_id);
          -- V1 persistent direct-call audit, no SDP/ICE or recordings stored.
          CREATE TABLE IF NOT EXISTS cc_dm_call_history (
            call_id VARCHAR(32) PRIMARY KEY,
            caller_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            callee_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            mode VARCHAR(8) NOT NULL CHECK (mode IN ('voice','video')),
            status VARCHAR(12) NOT NULL DEFAULT 'ringing'
              CHECK (status IN ('ringing','missed','declined','answered','canceled')),
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            answered_at TIMESTAMPTZ
          );
          CREATE INDEX IF NOT EXISTS cc_dm_call_history_callee_date
            ON cc_dm_call_history(callee_id, created_at DESC);
          CREATE TABLE IF NOT EXISTS login_sessions (
            token_hash VARCHAR(64) PRIMARY KEY,
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            expires_at TIMESTAMPTZ NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
          );
          CREATE INDEX IF NOT EXISTS login_sessions_user_id ON login_sessions(user_id);
          CREATE TABLE IF NOT EXISTS messages (
            id BIGSERIAL PRIMARY KEY,
            author_id VARCHAR(32) NOT NULL REFERENCES users(id),
            recipient_id VARCHAR(32) REFERENCES users(id),
            body TEXT NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
          );
          CREATE INDEX IF NOT EXISTS messages_author_id ON messages(author_id);
          CREATE INDEX IF NOT EXISTS messages_recipient_id ON messages(recipient_id);
          -- Sender sees Sent when committed, Delivered after recipient session
          -- explicitly acknowledges receiving it, Seen after recipient opens the chat.
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS delivered_at TIMESTAMPTZ;
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS seen_at TIMESTAMPTZ;
          -- Keep message IDs stable so existing read receipts and chats survive edits.
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS edited_at TIMESTAMPTZ;
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMPTZ;
          -- Optional reply reference stays inside the same private conversation (API-enforced).
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS reply_to_id BIGINT REFERENCES messages(id) ON DELETE SET NULL;
          -- A transport retry with the same random key must never create a second message.
          -- Original content fingerprint stays immutable even if a message is edited later.
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS client_ref VARCHAR(32);
          ALTER TABLE messages ADD COLUMN IF NOT EXISTS client_payload_hash VARCHAR(64);
          CREATE UNIQUE INDEX IF NOT EXISTS cc_dm_unique_client_ref
            ON messages(author_id,client_ref) WHERE client_ref IS NOT NULL;
          CREATE TABLE IF NOT EXISTS cc_dm_reactions (
            message_id BIGINT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            emoji TEXT NOT NULL,
            PRIMARY KEY(message_id,user_id)
          );
          -- Delete for Me is a per-account visibility flag, NEVER a modification
          -- to the shared message body or recipient's copy. Works after re-login.
          CREATE TABLE IF NOT EXISTS message_hidden_for_user (
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            message_id BIGINT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
            hidden_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(user_id,message_id)
          );
          CREATE INDEX IF NOT EXISTS message_hidden_for_user_message
            ON message_hidden_for_user(message_id);
          -- Connections only exist after a receiver explicitly accepts a request.
          CREATE TABLE IF NOT EXISTS friend_requests (
            id BIGSERIAL PRIMARY KEY,
            sender_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            receiver_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            status VARCHAR(12) NOT NULL DEFAULT 'pending'
              CHECK(status IN ('pending','accepted','rejected')),
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            responded_at TIMESTAMPTZ,
            CHECK(sender_id <> receiver_id)
          );
          CREATE UNIQUE INDEX IF NOT EXISTS friend_requests_pending_pair
            ON friend_requests(LEAST(sender_id,receiver_id), GREATEST(sender_id,receiver_id))
            WHERE status='pending';
          CREATE INDEX IF NOT EXISTS friend_requests_receiver_status ON friend_requests(receiver_id,status);
          CREATE TABLE IF NOT EXISTS friendships (
            user_low VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            user_high VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(user_low,user_high),
            CHECK(user_low < user_high)
          );
          -- Blocks are directional, and do not reveal who has blocked whom.
          -- Never automatically reconnect friendships after an unblock.
          CREATE TABLE IF NOT EXISTS user_blocks (
            blocker_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            blocked_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(blocker_id,blocked_id),
            CHECK(blocker_id <> blocked_id)
          );
          CREATE INDEX IF NOT EXISTS user_blocks_blocked_idx ON user_blocks(blocked_id);
          -- Mute belongs to one account and does not change unread counts.
          CREATE TABLE IF NOT EXISTS muted_contacts (
            owner_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            peer_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(owner_id,peer_id),
            CHECK(owner_id <> peer_id)
          );
          -- Persistent per-conversation read markers. Safe to create on existing databases.
          CREATE TABLE IF NOT EXISTS chat_read_markers (
            reader_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            sender_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            last_read_id BIGINT NOT NULL DEFAULT 0,
            updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(reader_id,sender_id),
            CHECK(reader_id <> sender_id)
          );
          -- Invite-only groups. Group IDs are random, immutable and NOT passwords.
          CREATE TABLE IF NOT EXISTS cc_groups (
            id VARCHAR(24) PRIMARY KEY,
            name VARCHAR(64) NOT NULL,
            owner_id VARCHAR(32) NOT NULL REFERENCES users(id),
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
          );
          -- Existing group records are preserved when upgrading.
          ALTER TABLE cc_groups ADD COLUMN IF NOT EXISTS description VARCHAR(240) NOT NULL DEFAULT '';
          ALTER TABLE cc_groups ADD COLUMN IF NOT EXISTS photo_data TEXT NOT NULL DEFAULT '';
          -- Shared conversation wallpapers are independent from each user's personal theme.
          -- Each participant can update the shared DM wallpaper after accepted friendship.
          CREATE TABLE IF NOT EXISTS cc_dm_wallpapers (
            user_low VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            user_high VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            wallpaper_id VARCHAR(32) NOT NULL DEFAULT '',
            updated_by VARCHAR(32) NOT NULL REFERENCES users(id),
            updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(user_low,user_high)
          );
          CREATE INDEX IF NOT EXISTS cc_dm_wallpapers_high ON cc_dm_wallpapers(user_high);
          ALTER TABLE cc_groups ADD COLUMN IF NOT EXISTS wallpaper_id VARCHAR(32) NOT NULL DEFAULT '';
          CREATE TABLE IF NOT EXISTS cc_group_mutes (
            group_id VARCHAR(24) NOT NULL REFERENCES cc_groups(id) ON DELETE CASCADE,
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(group_id,user_id)
          );
          CREATE INDEX IF NOT EXISTS cc_group_mutes_user ON cc_group_mutes(user_id);
          CREATE TABLE IF NOT EXISTS cc_group_members (
            group_id VARCHAR(24) NOT NULL REFERENCES cc_groups(id) ON DELETE CASCADE,
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            role VARCHAR(8) NOT NULL CHECK(role IN ('owner','member')),
            joined_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(group_id,user_id)
          );
          CREATE INDEX IF NOT EXISTS cc_group_members_user ON cc_group_members(user_id);
          CREATE TABLE IF NOT EXISTS cc_group_invites (
            id BIGSERIAL PRIMARY KEY,
            group_id VARCHAR(24) NOT NULL REFERENCES cc_groups(id) ON DELETE CASCADE,
            inviter_id VARCHAR(32) NOT NULL REFERENCES users(id),
            invitee_id VARCHAR(32) NOT NULL REFERENCES users(id),
            status VARCHAR(10) NOT NULL DEFAULT 'pending'
              CHECK(status IN ('pending','accepted','declined')),
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            answered_at TIMESTAMPTZ,
            CHECK(inviter_id<>invitee_id)
          );
          CREATE UNIQUE INDEX IF NOT EXISTS cc_group_pending_invite
            ON cc_group_invites(group_id,invitee_id) WHERE status='pending';
          CREATE INDEX IF NOT EXISTS cc_group_invites_inbox
            ON cc_group_invites(invitee_id,status);
          CREATE TABLE IF NOT EXISTS cc_group_messages (
            id BIGSERIAL PRIMARY KEY,
            group_id VARCHAR(24) NOT NULL REFERENCES cc_groups(id) ON DELETE CASCADE,
            author_id VARCHAR(32) NOT NULL REFERENCES users(id),
            body TEXT NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
          );
          CREATE INDEX IF NOT EXISTS cc_group_messages_by_group
            ON cc_group_messages(group_id,id);
          -- Attachments store metadata only. Data lives in a separate, persistent,
          -- non-public Docker volume. Messages preserve DB visibility semantics.
          CREATE TABLE IF NOT EXISTS cc_attachments (
            id BIGSERIAL PRIMARY KEY,
            dm_message_id BIGINT UNIQUE REFERENCES messages(id) ON DELETE CASCADE,
            group_message_id BIGINT UNIQUE REFERENCES cc_group_messages(id) ON DELETE CASCADE,
            uploader_id VARCHAR(32) NOT NULL REFERENCES users(id),
            filename VARCHAR(120) NOT NULL,
            mime VARCHAR(100) NOT NULL,
            size_bytes BIGINT NOT NULL CHECK(size_bytes>0 AND size_bytes<=314572800),
            storage_name VARCHAR(40) NOT NULL UNIQUE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            CHECK ((dm_message_id IS NOT NULL) <> (group_message_id IS NOT NULL))
          );
          CREATE INDEX IF NOT EXISTS cc_attachments_dm ON cc_attachments(dm_message_id);
          CREATE INDEX IF NOT EXISTS cc_attachments_group ON cc_attachments(group_message_id);
          CREATE TABLE IF NOT EXISTS cc_attachment_uploads (
            upload_id VARCHAR(32) PRIMARY KEY,
            uploader_id VARCHAR(32) NOT NULL REFERENCES users(id),
            scope VARCHAR(6) NOT NULL CHECK(scope IN ('dm','group')),
            recipient_id VARCHAR(32),
            group_id VARCHAR(24),
            filename VARCHAR(120) NOT NULL,
            size_bytes BIGINT NOT NULL CHECK(size_bytes>0 AND size_bytes<=314572800),
            uploaded_bytes BIGINT NOT NULL DEFAULT 0,
            updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            CHECK ((scope='dm' AND recipient_id IS NOT NULL AND group_id IS NULL) OR
                   (scope='group' AND group_id IS NOT NULL AND recipient_id IS NULL))
          );
          CREATE INDEX IF NOT EXISTS cc_attachment_uploads_owner ON cc_attachment_uploads(uploader_id);
          -- Group messages support edits just like direct messages.
          -- Add the missing field for both fresh and existing databases.
          ALTER TABLE cc_group_messages ADD COLUMN IF NOT EXISTS edited_at TIMESTAMPTZ;
          ALTER TABLE cc_group_messages ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMPTZ;
          ALTER TABLE cc_group_messages ADD COLUMN IF NOT EXISTS reply_to_id BIGINT REFERENCES cc_group_messages(id) ON DELETE SET NULL;
          ALTER TABLE cc_group_messages ADD COLUMN IF NOT EXISTS client_ref VARCHAR(32);
          ALTER TABLE cc_group_messages ADD COLUMN IF NOT EXISTS client_payload_hash VARCHAR(64);
          CREATE UNIQUE INDEX IF NOT EXISTS cc_group_unique_client_ref
            ON cc_group_messages(author_id,client_ref) WHERE client_ref IS NOT NULL;
          CREATE TABLE IF NOT EXISTS cc_group_reactions (
            message_id BIGINT NOT NULL REFERENCES cc_group_messages(id) ON DELETE CASCADE,
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            emoji TEXT NOT NULL,
            PRIMARY KEY(message_id,user_id)
          );
          -- Delete for Me is scoped to the account, not to the whole group.
          CREATE TABLE IF NOT EXISTS cc_group_message_hides (
            message_id BIGINT NOT NULL REFERENCES cc_group_messages(id) ON DELETE CASCADE,
            user_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            hidden_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY(message_id,user_id)
          );
          CREATE INDEX IF NOT EXISTS cc_group_message_hides_user
            ON cc_group_message_hides(user_id,message_id);
          CREATE TABLE IF NOT EXISTS cc_group_read_markers (
            group_id VARCHAR(24) NOT NULL REFERENCES cc_groups(id) ON DELETE CASCADE,
            reader_id VARCHAR(32) NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            last_read_id BIGINT NOT NULL DEFAULT 0,
            PRIMARY KEY(group_id,reader_id)
          );
          -- Existing read acknowledgements predate receipts. Preserve their meaning.
          UPDATE messages m
             SET seen_at=COALESCE(m.seen_at,r.updated_at),
                 delivered_at=COALESCE(m.delivered_at,r.updated_at)
            FROM chat_read_markers r
           WHERE m.author_id=r.sender_id AND m.recipient_id=r.reader_id
             AND m.id <= r.last_read_id
             AND (m.seen_at IS NULL OR m.delivered_at IS NULL);
        )SQL");
    }
    struct User {string id,name,publicId;};
    string createSession(Database& db,const User& person) {
        // Cap outstanding sessions to ten per account. Lock the user row so
        // parallel logins cannot race around the cap. Keep the newest nine
        // existing sessions before adding the new device.
        const auto token=secureRandom(32);
        db.query("BEGIN");
        try {
            db.query("SELECT id FROM users WHERE id=$1 FOR UPDATE",{person.id});
            db.query("DELETE FROM login_sessions WHERE user_id=$1 AND expires_at<=now()",{person.id});
            db.query(R"SQL(
                DELETE FROM login_sessions
                WHERE user_id=$1 AND token_hash NOT IN (
                    SELECT token_hash FROM login_sessions WHERE user_id=$1
                    ORDER BY created_at DESC, token_hash DESC LIMIT 9
                )
            )SQL",{person.id});
            db.query("INSERT INTO login_sessions(token_hash,user_id,expires_at) VALUES($1,$2,now()+INTERVAL '7 days')",{digest(token),person.id});
            db.query("COMMIT");
        } catch(...) {
            try {db.query("ROLLBACK");}catch(...){}
            throw;
        }
        return "{\"token\":"+json(token)+",\"id\":"+json(person.id)+",\"name\":"+json(person.name)+",\"publicId\":"+json(person.publicId)+"}";
    }
    string signup(const Fields& fields) {
        auto username=fields.count("name")?fields.at("name"):"";
        auto email=fields.count("email")?fields.at("email"):"";
        auto password=fields.count("password")?fields.at("password"):"";
        if(!validUsername(username))throw Problem{400,"Username must be 3–24 letters, numbers, or underscores."};
        if(!validEmail(email))throw Problem{400,"Enter a valid email address."};
        if(!validPassword(password))throw Problem{400,"Password must be between 10 and 128 characters."};
        Database db;const auto id=secureRandom(12),salt=secureRandom(16),publicId=publicIdFromRandom();
        db.query("INSERT INTO users(id,username,email,salt,password_hash,last_seen,public_id) VALUES($1,$2,$3,$4,$5,now(),$6)",
                {id,username,email,salt,passwordHash(password,salt),publicId});
        return createSession(db,{id,username,publicId});
    }
    string login(const Fields& fields) {
        const auto identity=fields.count("identity")?fields.at("identity"):"";
        const auto password=fields.count("password")?fields.at("password"):"";
        if(identity.empty()||identity.size()>254||password.empty()||password.size()>128)throw Problem{401,"Invalid username/email or password"};
        Database db;
        auto result=db.query("SELECT id,username,salt,password_hash,COALESCE(locked_until>now(),false),public_id FROM users WHERE lower(username)=lower($1) OR lower(email)=lower($1) LIMIT 1",{identity});
        if(PQntuples(result.get())==0) {
            // A dummy expensive hash reduces the user-enumeration timing difference.
            passwordHash(password,"00000000000000000000000000000000");
            throw Problem{401,"Invalid username/email or password"};
        }
        if(Database::field(result.get(),0,4)=="t")throw Problem{429,"Too many attempts. Try again after 15 minutes."};
        const User me{Database::field(result.get(),0,0),Database::field(result.get(),0,1),Database::field(result.get(),0,5)};
        bool match=constantEqual(passwordHash(password,Database::field(result.get(),0,2)),Database::field(result.get(),0,3));
        if(!match) {
            db.query("UPDATE users SET failed_attempts=CASE WHEN locked_until IS NOT NULL AND locked_until<=now() THEN 1 ELSE failed_attempts+1 END, "
                     "locked_until=CASE WHEN locked_until IS NOT NULL AND locked_until<=now() THEN NULL "
                     "WHEN failed_attempts+1>=5 THEN now()+INTERVAL '15 minutes' ELSE NULL END WHERE id=$1",{me.id});
            throw Problem{401,"Invalid username/email or password"};
        }
        db.query("UPDATE users SET failed_attempts=0,locked_until=NULL,last_seen=now() WHERE id=$1",{me.id});
        return createSession(db,me);
    }
    User authenticated(Database& db,const Request& r) {
        const auto h=r.headers.find("authorization");
        if(h==r.headers.end()||h->second.rfind("Bearer ",0)!=0)throw Problem{401,"Sign in to continue."};
        const auto token=h->second.substr(7);
        if(token.size()!=64||token.find_first_not_of("0123456789abcdef")!=string::npos)throw Problem{401,"Session expired. Sign in again."};
        auto result=db.query("SELECT u.id,u.username,u.public_id FROM login_sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=$1 AND s.expires_at>now()",{digest(token)});
        if(PQntuples(result.get())==0)throw Problem{401,"Session expired. Sign in again."};
        return {Database::field(result.get(),0,0),Database::field(result.get(),0,1),Database::field(result.get(),0,2)};
    }
    bool isBlockedEitherWay(Database& db,const string& left,const string& right) {
        auto result=db.query(R"SQL(
          SELECT 1 FROM user_blocks
          WHERE (blocker_id=$1 AND blocked_id=$2)
             OR (blocker_id=$2 AND blocked_id=$1) LIMIT 1
        )SQL",{left,right});
        return PQntuples(result.get())>0;
    }
    bool areFriends(Database& db,const string& left,const string& right) {
        auto rows=db.query(R"SQL(
            SELECT 1 FROM friendships f
            WHERE f.user_low=LEAST($1,$2) AND f.user_high=GREATEST($1,$2)
              AND NOT EXISTS (
                SELECT 1 FROM user_blocks b
                WHERE (b.blocker_id=$1 AND b.blocked_id=$2)
                   OR (b.blocker_id=$2 AND b.blocked_id=$1))
        )SQL",{left,right});
        return PQntuples(rows.get())==1;
    }
    void logout(Database& db,const Request& r) {
        const auto authorization=r.headers.at("authorization");
        db.query("DELETE FROM login_sessions WHERE token_hash=$1",{digest(authorization.substr(7))});
    }
};

// Short-lived, account-scoped change signals. No message text, account IDs,
// or bearer tokens are included in the event payload. The private /api/state
// endpoint remains the only way to fetch message contents.
class EventHub {
    std::mutex lock;
    std::condition_variable changed;
    std::map<string,std::uint64_t> versions;
public:
    std::uint64_t current(const string& id) {
        std::lock_guard<std::mutex> guard(lock);
        return versions[id];
    }
    std::pair<std::uint64_t,bool> waitFor(const string& id,std::uint64_t since,
                                           std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> guard(lock);
        changed.wait_for(guard,timeout,[&]{return versions[id]!=since;});
        const auto latest=versions[id];
        return {latest,latest!=since};
    }
    void publish(const string& id) {
        if(id.empty())return;
        {
            std::lock_guard<std::mutex> guard(lock);
            ++versions[id];
        }
        changed.notify_all();
    }
};

// ============================================================================
// ChatConnect Calls V1: ephemeral, authenticated WebRTC signaling.
// Only ICE/SDP signaling transits the C++ process; media is browser-to-browser
// (or relayed through a separately configured TURN service). No call recordings.
// ============================================================================
class CallHub {
public:
    struct Member {string id,name,status; long long updated;};
    struct Room {
        string id,scope,thread,owner,mode;
        long long created;
        std::map<string,Member> members;
    };
private:
    struct Signal {std::uint64_t seq;long long at;string call,from,kind,payload;};
    EventHub& events;
    std::mutex lock;
    std::map<string,Room> rooms;
    std::map<string,string> byUser;
    std::map<string,std::deque<Signal>> queues;
    std::map<string,std::uint64_t> counters;
    void push(const string& to,const string& call,const string& from,const string& kind,const string& payload="") {
        auto& q=queues[to];
        q.push_back({++counters[to],now(),call,from,kind,payload});
        while(q.size()>256)q.pop_front();
        events.publish(to);
    }
    void discard(const string& id) {
        auto it=rooms.find(id);if(it==rooms.end())return;
        for(const auto& p:it->second.members){
            if(byUser[p.first]==id)byUser.erase(p.first);
        }
        rooms.erase(it);
    }
    void sweep() {
        const auto ts=now();
        for(auto& [user,q]:queues){
            while(!q.empty() && ts-q.front().at>90)q.pop_front();
        }
        std::vector<string> remove;
        for(auto& [id,room]:rooms){
            std::vector<string> expired;
            for(const auto& [uid,p]:room.members){
                if((p.status=="invited" && ts-room.created>65) ||
                   (p.status=="active" && ts-p.updated>40) ||
                   ts-room.created>14400)expired.push_back(uid);
            }
            for(const auto& uid:expired){
                room.members.erase(uid);
                if(byUser.count(uid) && byUser.at(uid)==id)byUser.erase(uid);
                for(const auto& p:room.members)push(p.first,id,uid,"left");
            }
            // Do not hold an orphaned call open after everybody has left.
            if(room.members.empty())remove.push_back(id);
            else if(room.members.size()==1 && room.members.begin()->second.status=="active" &&
                    ts-room.created>65 && room.scope=="dm"){
                const auto only=room.members.begin()->first;
                push(only,id,"","ended");remove.push_back(id);
            }
        }
        for(const auto& id:remove)discard(id);
    }
    Room& owned(const string& call,const string& user) {
        auto it=rooms.find(call);
        if(it==rooms.end()||!it->second.members.count(user) ||
            !byUser.count(user)||byUser.at(user)!=call)
            throw Problem{404,"Call no longer available."};
        return it->second;
    }
    static string view(const Room& room,const string& user){
        std::ostringstream out;
        out<<"{\"id\":"<<json(room.id)<<",\"scope\":"<<json(room.scope)
           <<",\"thread\":"<<json(room.thread)<<",\"mode\":"<<json(room.mode)
           <<",\"owner\":"<<json(room.owner)<<",\"created\":"<<room.created
           <<",\"myStatus\":"<<json(room.members.at(user).status)<<",\"members\":[";
        bool first=true;
        for(const auto& p:room.members){
            if(!first)out<<',';first=false;
            out<<"{\"id\":"<<json(p.second.id)<<",\"name\":"<<json(p.second.name)
               <<",\"status\":"<<json(p.second.status)<<"}";
        }
        out<<"]}";return out.str();
    }
public:
    explicit CallHub(EventHub& e):events(e){}
    string start(const string& user,const string& scope,const string& thread,
                 const string& mode,const std::vector<std::pair<string,string>>& people){
        std::lock_guard<std::mutex> guard(lock);sweep();
        if(byUser.count(user))throw Problem{409,"Finish your current call first."};
        if(people.size()<2||people.size()>4)throw Problem{400,"Calls support 2 to 4 participants."};
        for(const auto& p:people)if(byUser.count(p.first))
            throw Problem{409,"Someone in this call is busy. Try again later."};
        const string id=secureRandom(16);
        Room room{id,scope,thread,user,mode,now(),{}};
        bool hasCaller=false;
        for(const auto& p:people){
            if(room.members.count(p.first))throw Problem{400,"Duplicate call member."};
            room.members.emplace(p.first,Member{p.first,p.second,p.first==user?"active":"invited",now()});
            if(p.first==user)hasCaller=true;
        }
        if(!hasCaller)throw Problem{403,"Caller must be a participant."};
        rooms.emplace(id,room);
        for(const auto& p:people){byUser[p.first]=id;if(p.first!=user)push(p.first,id,user,"ring");}
        return view(room,user);
    }
    // Only call identifier/scope are exposed here. Caller must separately check
    // their current friendship/group membership before mutating/polling the call.
    string currentId(const string& user){
        std::lock_guard<std::mutex> guard(lock);sweep();
        auto it=byUser.find(user);return it==byUser.end()?"":it->second;
    }
    string context(const string& user){
        std::lock_guard<std::mutex> guard(lock);sweep();
        auto it=byUser.find(user);if(it==byUser.end())return "";
        auto room=rooms.find(it->second);if(room==rooms.end())return "";
        if(room->second.scope=="dm"){
            for(const auto& p:room->second.members)if(p.first!=user)return "dm:"+p.first;
        }
        return room->second.scope+":"+room->second.thread;
    }
    string contextFor(const string& user,const string& id){
        std::lock_guard<std::mutex> guard(lock);sweep();
        auto& room=owned(id,user);if(room.scope=="dm"){
            for(const auto& p:room.members)if(p.first!=user)return "dm:"+p.first;
        }
        return room.scope+":"+room.thread;
    }
    string poll(const string& user,std::uint64_t cursor){
        std::lock_guard<std::mutex> guard(lock);sweep();
        string active="null";
        auto found=byUser.find(user);
        if(found!=byUser.end() && rooms.count(found->second)){
            auto& room=rooms.at(found->second);
            auto& m=room.members.at(user);
            if(m.status=="active")m.updated=now();
            active=view(room,user);
        }
        std::ostringstream out;
        out<<"{\"call\":"<<active<<",\"cursor\":"<<counters[user]<<",\"events\":[";
        bool first=true;
        auto it=queues.find(user);
        if(it!=queues.end())for(const auto& ev:it->second){
            if(ev.seq<=cursor)continue;
            if(!first)out<<',';first=false;
            out<<"{\"seq\":"<<ev.seq<<",\"call\":"<<json(ev.call)
               <<",\"from\":"<<json(ev.from)<<",\"kind\":"<<json(ev.kind)
               <<",\"payload\":"<<json(ev.payload)<<"}";
        }
        out<<"]}";return out.str();
    }
    string decision(const string& user,const string& id,bool accept){
        std::lock_guard<std::mutex> guard(lock);sweep();
        auto& room=owned(id,user);
        if(room.members.at(user).status!="invited")throw Problem{409,"Call invite already answered."};
        if(accept){
            room.members.at(user).status="active";
            room.members.at(user).updated=now();
            for(const auto& p:room.members)if(p.first!=user)push(p.first,id,user,"joined");
            return view(room,user);
        }
        room.members.erase(user);byUser.erase(user);
        for(const auto& p:room.members)push(p.first,id,user,"declined");
        if(room.scope=="dm"){
            for(const auto& p:room.members)push(p.first,id,user,"ended");
            discard(id);
        }
        return "{}";
    }
    string leave(const string& user,const string& id){
        std::lock_guard<std::mutex> guard(lock);sweep();
        auto& room=owned(id,user);
        room.members.erase(user);byUser.erase(user);
        for(const auto& p:room.members)push(p.first,id,user,room.scope=="dm"?"ended":"left");
        if(room.scope=="dm")discard(id);
        else if(room.members.empty())discard(id);
        return "{}";
    }
    string signal(const string& user,const string& id,const string& to,
                  const string& kind,const string& payload){
        std::lock_guard<std::mutex> guard(lock);sweep();
        auto& room=owned(id,user);
        auto src=room.members.find(user),dst=room.members.find(to);
        if(src->second.status!="active" || dst==room.members.end() ||
           dst->second.status!="active" || to==user)
            throw Problem{403,"Only connected call participants can exchange signals."};
        if((kind!="offer"&&kind!="answer"&&kind!="ice") || payload.empty() ||payload.size()>48000)
            throw Problem{400,"Invalid call signaling message."};
        src->second.updated=now();
        push(to,id,user,kind,payload);
        return "{}";
    }
};

class ChatRoom {
    Storage store;
    EventHub events;
    CallHub calls{events};
    EntryRateLimiter callLimiter;
    std::mutex mediaMutex;
    EntryRateLimiter uploadInitLimiter;
    std::mutex mutex;
    struct TypingState {string name,to,group;long long updated;};
    // Ephemeral only: typing content is never stored in PostgreSQL.
    std::map<string,TypingState> typing; // user id -> one active friend/group thread
    EntryRateLimiter typingLimiter;
    void expire() {
        for(auto i=typing.begin();i!=typing.end();) {
            if(now()-i->second.updated>4)i=typing.erase(i);else ++i;
        }
    }
    bool isGroupMember(Database& db,const string& group,const string& user) {
        auto rows=db.query("SELECT 1 FROM cc_group_members WHERE group_id=$1 AND user_id=$2",{group,user});
        return PQntuples(rows.get())!=0;
    }
    bool isGroupOwner(Database& db,const string& group,const string& user) {
        auto rows=db.query("SELECT 1 FROM cc_groups WHERE id=$1 AND owner_id=$2",{group,user});
        return PQntuples(rows.get())!=0;
    }
    void publishGroup(Database& db,const string& group) {
        // Send only account-scoped change signals, never message contents.
        auto rows=db.query("SELECT user_id FROM cc_group_members WHERE group_id=$1",{group});
        for(int i=0;i<PQntuples(rows.get());++i)events.publish(Database::field(rows.get(),i,0));
    }
    void removeStaleUploadParts(Database& db) {
        // Called while holding mediaMutex. Deleting the DB rows alone leaves
        // expired .part files consuming the media volume until restart.
        auto expired=db.query("DELETE FROM cc_attachment_uploads WHERE updated_at<now()-INTERVAL '24 hours' RETURNING upload_id");
        for(int i=0;i<PQntuples(expired.get());++i) {
            const string id=Database::field(expired.get(),i,0);
            if(!validUploadId(id))continue;
            std::error_code ec;
            std::filesystem::remove(CC_MEDIA_DIR/(id+".part"),ec);
            if(ec)std::cerr<<"Expired upload cleanup failed: "<<ec.message()<<'\n';
        }
    }
public:
    struct AttachmentDownload {string name,mime,path;std::uint64_t size;};
    AttachmentDownload downloadInfo(Request& r) {
        const auto data=fields(r.body);
        // A browser's native download form cannot set Authorization headers;
        // the token is POSTed in the body, never exposed in URL/history/logs.
        if(!r.headers.count("authorization") && data.count("download_token"))
            r.headers["authorization"]="Bearer "+data.at("download_token");
        Database db;
        const auto me=store.authenticated(db,r);
        const string id=data.count("id")?data.at("id"):"";
        if(!validSqlNumber(id))throw Problem{400,"Invalid attachment ID."};
        auto result=db.query(R"SQL(
              SELECT a.filename,a.mime,a.storage_name,a.size_bytes
              FROM cc_attachments a
              WHERE a.id=$2::BIGINT AND (
                EXISTS (
                  SELECT 1 FROM messages m
                  JOIN friendships f ON f.user_low=LEAST(m.author_id,m.recipient_id)
                                    AND f.user_high=GREATEST(m.author_id,m.recipient_id)
                  WHERE m.id=a.dm_message_id AND m.deleted_at IS NULL
                    AND m.recipient_id IS NOT NULL AND (m.author_id=$1 OR m.recipient_id=$1)
                    AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE
                      (b.blocker_id=m.author_id AND b.blocked_id=m.recipient_id)
                       OR (b.blocker_id=m.recipient_id AND b.blocked_id=m.author_id))
                    AND NOT EXISTS(SELECT 1 FROM message_hidden_for_user h
                        WHERE h.message_id=m.id AND h.user_id=$1)
                ) OR EXISTS (
                  SELECT 1 FROM cc_group_messages gm
                  JOIN cc_group_members member ON member.group_id=gm.group_id AND member.user_id=$1
                  WHERE gm.id=a.group_message_id AND gm.deleted_at IS NULL
                    AND gm.created_at>=member.joined_at
                    AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                        WHERE h.message_id=gm.id AND h.user_id=$1)
                    AND NOT EXISTS(SELECT 1 FROM user_blocks b WHERE
                      (b.blocker_id=$1 AND b.blocked_id=gm.author_id)
                       OR (b.blocker_id=gm.author_id AND b.blocked_id=$1))
                )
              )
            )SQL",{me.id,id});
        if(!PQntuples(result.get()))throw Problem{404,"Attachment not available for this account."};
        const string stored=Database::field(result.get(),0,2);
        if(stored.size()!=36 || stored.substr(32)!=".bin" || !validUploadId(stored.substr(0,32)))
            throw Problem{503,"Invalid stored attachment reference."};
        const auto path=CC_MEDIA_DIR/stored;
        std::error_code ec;
        if(!std::filesystem::exists(path,ec) ||
           std::filesystem::file_size(path,ec)!=std::stoull(Database::field(result.get(),0,3)) || ec)
            throw Problem{503,"Attachment file missing or damaged."};
        const auto n=std::stoull(Database::field(result.get(),0,3));
        if(r.path=="/api/attachments/preview" &&
            (n>8ULL*1024*1024 || Database::field(result.get(),0,1).rfind("image/",0)!=0))
            throw Problem{413,"Preview only supports photos up to 8 MB."};
        if(r.path=="/api/attachments/audio" &&
            (n>CC_VOICE_LIMIT || Database::field(result.get(),0,1).rfind("audio/",0)!=0))
            throw Problem{413,"Voice playback supports audio up to 300 MiB."};
        return {Database::field(result.get(),0,0),Database::field(result.get(),0,1),path.string(),n};
    }
    ChatRoom() {
        store.initialize();
        std::error_code ec;
        if(!std::filesystem::is_directory(CC_MEDIA_DIR,ec))
            throw Problem{503,"Persistent media volume is missing. Apply Compose/Dockerfile patch."};
        // On startup only: safely reclaim orphan files whose metadata was
        // deleted (Delete for Everyone / group deletion) or abandoned uploads.
        // Never remove an active upload's .part file.
        try {
            Database db;
            db.query("DELETE FROM cc_attachment_uploads WHERE updated_at<now()-INTERVAL '24 hours'");
            auto tracked=db.query(R"SQL(
                SELECT storage_name FROM cc_attachments
                UNION ALL SELECT upload_id||'.part' FROM cc_attachment_uploads
            )SQL");
            std::set<string> keep;
            for(int i=0;i<PQntuples(tracked.get());++i)
                keep.insert(Database::field(tracked.get(),i,0));
            for(const auto& item:std::filesystem::directory_iterator(CC_MEDIA_DIR)){
                if(!item.is_regular_file())continue;
                const auto name=item.path().filename().string();
                const bool generated=name.size()==37 && name.substr(32)==".part" && validUploadId(name.substr(0,32));
                const bool finished=name.size()==36 && name.substr(32)==".bin" && validUploadId(name.substr(0,32));
                if((generated||finished) && !keep.count(name))
                    std::filesystem::remove(item.path(),ec);
            }
        }catch(const Problem&){throw;}
         catch(const std::exception& e){std::cerr<<"Media cleanup warning: "<<e.what()<<'\n';}
    }
    string api(Request& r) {
        auto data=fields(r.body);
        if(r.path=="/api/signup"&&r.method=="POST")return store.signup(data);
        if(r.path=="/api/login"&&r.method=="POST")return store.login(data);
        Database db;
        const auto me=store.authenticated(db,r);
        // Calls V1. Bearer auth is checked above; never trust caller IDs in forms.
        // SDP/ICE are short-lived in-memory signaling, not media recordings.
        if(r.path=="/api/calls/config" && r.method=="GET") {
            string stun="stun:stun.l.google.com:19302";
            if(const char* configured=std::getenv("CC_STUN_URL")) {
                if(string(configured).size()<240)stun=configured;
            }
            std::ostringstream out;
            out<<"{\"iceServers\":[";
            bool previous=false;
            if(!stun.empty() && stun.rfind("stun:",0)==0) {out<<"{\"urls\":"<<json(stun)<<"}";previous=true;}
            const char* url=std::getenv("CC_TURN_URL");
            const char* secret=std::getenv("CC_TURN_SECRET");
            if(url&&secret&&string(url).size()<240&&string(secret).size()>=20&&
              (string(url).rfind("turn:",0)==0||string(url).rfind("turns:",0)==0)){
                const string username=std::to_string(now()+3600)+":"+me.id;
                unsigned int count=0;
                unsigned char digestBytes[EVP_MAX_MD_SIZE];
                HMAC(EVP_sha1(),secret,(int)std::strlen(secret),
                    (const unsigned char*)username.data(),username.size(),digestBytes,&count);
                std::vector<unsigned char> base64(4*((count+2)/3)+1);
                EVP_EncodeBlock(base64.data(),digestBytes,count);
                if(previous)out<<",";
                out<<"{\"urls\":"<<json(url)<<",\"username\":"<<json(username)
                   <<",\"credential\":"<<json((const char*)base64.data())<<"}";
            }
            out<<"]}";return out.str();
        }
        if(r.path=="/api/calls/history" && r.method=="GET") {
            // Callee-scoped only: never trust user IDs from the request.
            db.query("UPDATE cc_dm_call_history SET status='missed' "
                     "WHERE status='ringing' AND callee_id=$1 AND created_at < now()-INTERVAL '75 seconds'",{me.id});
            auto rows=db.query(R"SQL(
                SELECT h.call_id,u.username,h.mode,h.status,
                       EXTRACT(EPOCH FROM h.created_at)::BIGINT::TEXT
                FROM cc_dm_call_history AS h
                JOIN users AS u ON u.id=h.caller_id
                WHERE h.callee_id=$1
                ORDER BY h.created_at DESC LIMIT 30
            )SQL",{me.id});
            std::ostringstream out;out<<"{\"calls\":[";
            for(int i=0;i<PQntuples(rows.get());++i){
                if(i)out<<',';
                out<<"{\"id\":"<<json(Database::field(rows.get(),i,0))
                   <<",\"from\":"<<json(Database::field(rows.get(),i,1))
                   <<",\"mode\":"<<json(Database::field(rows.get(),i,2))
                   <<",\"status\":"<<json(Database::field(rows.get(),i,3))
                   <<",\"created\":"<<json(Database::field(rows.get(),i,4))<<"}";
            }
            out<<"]}";return out.str();
        }
        if(r.path=="/api/calls/start" && r.method=="POST") {
            callLimiter.enforce("call-start:"+me.id,8,std::chrono::minutes(1));
            const string to=data["to"],group=data["group_id"];
            const string mode=data["mode"];
            if(mode!="voice"&&mode!="video")throw Problem{400,"Choose voice or video."};
            if(to.empty()==group.empty())throw Problem{400,"Select one friend or group."};
            std::vector<std::pair<string,string>> people;
            if(!to.empty()) {
                if(to==me.id || !store.areFriends(db,me.id,to))
                    throw Problem{403,"Only connected friends can call each other."};
                auto peer=db.query("SELECT username FROM users WHERE id=$1",{to});
                if(!PQntuples(peer.get()))throw Problem{404,"Friend not found."};
                people={{me.id,me.name},{to,Database::field(peer.get(),0,0)}};
            } else {
                if(!validGroupId(group)||!isGroupMember(db,group,me.id))
                    throw Problem{403,"Group membership required to call."};
                auto members=db.query("SELECT m.user_id,u.username FROM cc_group_members m "
                   "JOIN users u ON u.id=m.user_id WHERE m.group_id=$1 ORDER BY m.joined_at LIMIT 5",{group});
                if(PQntuples(members.get())>4)throw Problem{400,"Group calls support up to 4 members in V1."};
                for(int i=0;i<PQntuples(members.get());++i)
                   people.push_back({Database::field(members.get(),i,0),Database::field(members.get(),i,1)});
            }
            const string reply=calls.start(me.id,to.empty()?"group":"dm",to.empty()?group:to,mode,people);
            if(!to.empty()){
                const string id=calls.currentId(me.id);
                try {
                    db.query("INSERT INTO cc_dm_call_history(call_id,caller_id,callee_id,mode,status) "
                             "VALUES($1,$2,$3,$4,'ringing') ON CONFLICT(call_id) DO NOTHING",
                             {id,me.id,to,mode});
                }catch(...){
                    try{calls.leave(me.id,id);}catch(...){}
                    throw;
                }
            }
            return reply;
        }
        if(r.path.rfind("/api/calls/",0)==0){
            if(r.method!="POST")throw Problem{405,"POST required."};
            callLimiter.enforce("call-action:"+me.id,240,std::chrono::minutes(1));
            const string call=data["call_id"];
            const string operation=r.path;
            const string ctx=operation=="/api/calls/poll"?calls.context(me.id):calls.contextFor(me.id,call);
            if(!ctx.empty()){
                const auto at=ctx.find(':');
                const string scope=ctx.substr(0,at),thread=ctx.substr(at+1);
                if((scope=="dm" && !store.areFriends(db,me.id,thread)) ||
                   (scope=="group" && !isGroupMember(db,thread,me.id))){
                    // Authorization changed during the call; immediately leave it.
                    const string invalidId=call.empty()?calls.currentId(me.id):call;
                    if(!invalidId.empty())try{calls.leave(me.id,invalidId);}catch(...){}
                    throw Problem{403,"Call permission no longer valid."};
                }
            }
            if(operation=="/api/calls/poll"){
                const string raw=data["cursor"];
                if(raw.size()>20 || (!raw.empty()&&raw.find_first_not_of("0123456789")!=string::npos))
                    throw Problem{400,"Invalid call cursor."};
                std::uint64_t cursor=0;
                if(!raw.empty())try{cursor=std::stoull(raw);}catch(...){throw Problem{400,"Invalid call cursor."};}
                return calls.poll(me.id,cursor);
            }
            if(call.size()!=32||call.find_first_not_of("0123456789abcdef")!=string::npos)
                throw Problem{400,"Invalid call ID."};
            if(operation=="/api/calls/decision"){
                const bool accepted=data["accept"]=="1";
                const string reply=calls.decision(me.id,call,accepted);
                try {
                    db.query("UPDATE cc_dm_call_history SET status=$3, "
                             "answered_at=CASE WHEN $3='answered' THEN now() ELSE answered_at END "
                             "WHERE call_id=$1 AND callee_id=$2 AND status='ringing'",
                             {call,me.id,accepted?"answered":"declined"});
                }catch(...){std::cerr<<"Call-history decision update unavailable\n";}
                return reply;
            }
            if(operation=="/api/calls/leave"){
                const string reply=calls.leave(me.id,call);
                try {
                    db.query("UPDATE cc_dm_call_history SET status='canceled' "
                             "WHERE call_id=$1 AND caller_id=$2 AND status='ringing'",
                             {call,me.id});
                }catch(...){std::cerr<<"Call-history cancel update unavailable\n";}
                return reply;
            }
            if(operation=="/api/calls/signal"){
                // Additional recipient check even for calls from a group.
                const string recipient=data["to"];
                if(ctx.rfind("group:",0)==0 && !isGroupMember(db,ctx.substr(6),recipient))
                    throw Problem{403,"Recipient no longer belongs to this group."};
                if(data["kind"]=="ice" && data["payload"].size()>4096)
                    throw Problem{413,"Oversized ICE candidate."};
                return calls.signal(me.id,call,recipient,data["kind"],data["payload"]);
            }
            throw Problem{404,"Unknown call action."};
        }

        // Two-way shared wallpaper: ONLY accepted, unblocked friends can change it.
        // One row for the unordered pair means both accounts see the same selection.
        if(r.path=="/api/wallpaper/dm" && r.method=="POST") {
            const string peer=data["peer_id"], wall=data["wallpaper_id"];
            if(!validChatWallpaper(wall))throw Problem{400,"Choose a built-in wallpaper."};
            if(peer.empty()||peer==me.id||!store.areFriends(db,me.id,peer))
                throw Problem{403,"Only connected friends can share a chat wallpaper."};
            db.query(R"SQL(
                INSERT INTO cc_dm_wallpapers(user_low,user_high,wallpaper_id,updated_by,updated_at)
                VALUES(LEAST($1::VARCHAR(32),$2::VARCHAR(32)),
                       GREATEST($1::VARCHAR(32),$2::VARCHAR(32)),
                       $3::VARCHAR(32),$1::VARCHAR(32),now())
                ON CONFLICT(user_low,user_high) DO UPDATE
                SET wallpaper_id=EXCLUDED.wallpaper_id,
                    updated_by=EXCLUDED.updated_by,updated_at=now()
            )SQL",{me.id,peer,wall});
            events.publish(me.id);events.publish(peer);
            return "{\"status\":\"saved\"}";
        }
        // Group wallpaper is shared with group members; only the owner can change it.
        if(r.path=="/api/wallpaper/group" && r.method=="POST") {
            const string group=data["group_id"],wall=data["wallpaper_id"];
            if(!validGroupId(group) || !validChatWallpaper(wall))
                throw Problem{400,"Invalid group or wallpaper."};
            auto updated=db.query("UPDATE cc_groups SET wallpaper_id=$3::VARCHAR(32) "
                                  "WHERE id=$1::VARCHAR(24) AND owner_id=$2::VARCHAR(32) "
                                  "RETURNING id",{group,me.id,wall});
            if(!PQntuples(updated.get()))throw Problem{403,"Only the group owner can change its shared wallpaper."};
            publishGroup(db,group);
            return "{\"status\":\"saved\"}";
        }
        // Attachment uploads use small authenticated chunks (512 KiB). No huge
        // Base64 request, huge PostgreSQL BYTEA, or browser file-sized buffer.
        if(r.path=="/api/attachments/init" && r.method=="POST") {
            // Per-authenticated-account protection; does not throttle 512 KiB
            // chunks or legitimate 300 MiB uploads once initialized.
            uploadInitLimiter.enforce("upload-init:"+me.id,30,std::chrono::minutes(1));
            const string scope=data["scope"],name=data["name"];
            const auto size=parseFileSize(data["size"]);
            const string extension=allowedAttachmentExtension(name);
            if(extension.empty())
                throw Problem{415,"Unsupported attachment extension."};
            if((extension==".webm" || extension==".ogg" || extension==".m4a") && size>CC_VOICE_LIMIT)
                throw Problem{413,"Voice notes must be 300 MiB or smaller."};
            string to,group;
            if(scope=="dm") {
                to=data["to"];
                if(to.empty()||to==me.id||!store.areFriends(db,me.id,to))
                    throw Problem{403,"Only connected friends can receive files."};
            } else if(scope=="group") {
                group=data["group_id"];
                if(!validGroupId(group))throw Problem{400,"Invalid group ID."};
                if(!isGroupMember(db,group,me.id))throw Problem{403,"Only group members can send files."};
            } else throw Problem{400,"Choose a friend or group for your attachment."};
            std::lock_guard<std::mutex> guard(mediaMutex);
            removeStaleUploadParts(db);
            auto used=db.query(R"SQL(
              SELECT COALESCE((SELECT SUM(size_bytes) FROM cc_attachments WHERE uploader_id=$1),0)
                   + COALESCE((SELECT SUM(size_bytes) FROM cc_attachment_uploads WHERE uploader_id=$1),0)
            )SQL",{me.id});
            std::uint64_t usedBytes=std::stoull(Database::field(used.get(),0,0));
            if(usedBytes>2ULL*1024*1024*1024-size)
                throw Problem{413,"Upload quota reached (2 GiB per account)."};
            auto count=db.query("SELECT count(*) FROM cc_attachment_uploads WHERE uploader_id=$1",{me.id});
            if(std::stoi(Database::field(count.get(),0,0))>=3)
                throw Problem{429,"Finish or cancel one of your active uploads."};
            std::error_code ec;
            auto available=std::filesystem::space(CC_MEDIA_DIR,ec);
            if(ec||available.available<size+(5*1024*1024))throw Problem{507,"Not enough free media storage."};
            const string id=secureRandom(16);
            db.query(R"SQL(
               INSERT INTO cc_attachment_uploads(upload_id,uploader_id,scope,recipient_id,group_id,filename,size_bytes)
               VALUES($1,$2,$3,NULLIF($4,''),NULLIF($5,''),$6,$7::BIGINT)
            )SQL",{id,me.id,scope,to,group,name,std::to_string(size)});
            std::ofstream part(CC_MEDIA_DIR/(id+".part"),std::ios::binary|std::ios::trunc);
            if(!part) {
                db.query("DELETE FROM cc_attachment_uploads WHERE upload_id=$1",{id});
                throw Problem{507,"Media volume is not writable."};
            }
            return "{\"uploadId\":"+json(id)+",\"maxBytes\":"+std::to_string(CC_FILE_LIMIT)+"}";
        }
        if(r.path=="/api/attachments/chunk" && r.method=="POST") {
            const string id=data["upload_id"];
            if(!validUploadId(id))throw Problem{400,"Invalid upload ID."};
            const auto offset=parseUploadOffset(data["offset"]);
            string encoded=data["data"];
            std::replace(encoded.begin(),encoded.end(),'-','+');
            std::replace(encoded.begin(),encoded.end(),'_','/');
            const auto chunk=decodeAttachmentChunk(encoded);
            std::lock_guard<std::mutex> guard(mediaMutex);
            auto rows=db.query("SELECT size_bytes,uploaded_bytes FROM cc_attachment_uploads WHERE upload_id=$1 AND uploader_id=$2",{id,me.id});
            if(!PQntuples(rows.get()))throw Problem{404,"Upload not found or not owned by you."};
            const std::uint64_t total=std::stoull(Database::field(rows.get(),0,0));
            const std::uint64_t done=std::stoull(Database::field(rows.get(),0,1));
            if(offset!=done)throw Problem{409,"Unexpected chunk offset. Resume from server progress."};
            if(chunk.size()>total-done)throw Problem{413,"Chunk exceeds file size."};
            const auto part=CC_MEDIA_DIR/(id+".part");
            std::error_code ec;
            if(!std::filesystem::exists(part,ec))throw Problem{503,"Temporary upload file unavailable."};
            if(std::filesystem::file_size(part,ec)!=done||ec)throw Problem{503,"Upload storage corrupted."};
            std::fstream output(part,std::ios::binary|std::ios::in|std::ios::out);
            if(!output)throw Problem{507,"Cannot write attachment data."};
            output.seekp(std::streamoff(done));
            output.write((const char*)chunk.data(),std::streamsize(chunk.size()));
            output.flush();
            if(!output)throw Problem{507,"Failed writing attachment data."};
            db.query("UPDATE cc_attachment_uploads SET uploaded_bytes=$3::BIGINT,updated_at=now() WHERE upload_id=$1 AND uploader_id=$2",
                {id,me.id,std::to_string(done+chunk.size())});
            return "{\"received\":"+std::to_string(done+chunk.size())+"}";
        }
        if(r.path=="/api/attachments/abort" && r.method=="POST") {
            const string id=data["upload_id"];
            if(!validUploadId(id))throw Problem{400,"Invalid upload ID."};
            std::lock_guard<std::mutex> guard(mediaMutex);
            auto removed=db.query("DELETE FROM cc_attachment_uploads WHERE upload_id=$1 AND uploader_id=$2 RETURNING upload_id",{id,me.id});
            if(!PQntuples(removed.get()))throw Problem{404,"Upload not found or not owned by you."};
            std::error_code ec; std::filesystem::remove(CC_MEDIA_DIR/(id+".part"),ec);
            return "{\"status\":\"aborted\"}";
        }
        if(r.path=="/api/attachments/complete" && r.method=="POST") {
            const string id=data["upload_id"];
            if(!validUploadId(id))throw Problem{400,"Invalid upload ID."};
            std::lock_guard<std::mutex> guard(mediaMutex);
            auto rows=db.query(R"SQL(
                SELECT scope,COALESCE(recipient_id,''),COALESCE(group_id,''),filename,size_bytes,uploaded_bytes
                FROM cc_attachment_uploads WHERE upload_id=$1 AND uploader_id=$2
            )SQL",{id,me.id});
            if(!PQntuples(rows.get()))throw Problem{404,"Upload not found."};
            const string scope=Database::field(rows.get(),0,0),to=Database::field(rows.get(),0,1),group=Database::field(rows.get(),0,2),name=Database::field(rows.get(),0,3);
            const auto total=std::stoull(Database::field(rows.get(),0,4));
            const auto uploaded=std::stoull(Database::field(rows.get(),0,5));
            if(uploaded!=total)throw Problem{409,"File upload incomplete."};
            if(scope=="dm" && !store.areFriends(db,me.id,to))throw Problem{403,"Friendship changed; file not sent."};
            if(scope=="group" && !isGroupMember(db,group,me.id))throw Problem{403,"Group membership changed; file not sent."};
            const auto temp=CC_MEDIA_DIR/(id+".part"),finalPath=CC_MEDIA_DIR/(id+".bin");
            std::error_code ec;
            if(!std::filesystem::exists(temp,ec) || std::filesystem::file_size(temp,ec)!=total || ec)
                throw Problem{503,"Uploaded file incomplete on disk."};
            const string mime=sniffAttachmentFile(name,temp,total);
            if(mime.empty())throw Problem{415,"Invalid file signature for selected type."};
            const string label=(mime.rfind("image/",0)==0?"📷 ":
                mime.rfind("audio/",0)==0?"🎙️ ":"📎 ")+name;
            db.query("BEGIN");
            try {
                Database::Result created;
                if(scope=="dm"){
                    // Parameter types and ownership are explicit, as in normal DM sends.
                    created=db.query(R"SQL(
                      INSERT INTO messages(author_id,recipient_id,body)
                      SELECT sender.id,receiver.id,$3::TEXT
                      FROM users AS sender JOIN users AS receiver
                        ON receiver.id=$2::VARCHAR(32)
                      WHERE sender.id=$1::VARCHAR(32)
                        AND EXISTS(SELECT 1 FROM friendships AS f WHERE
                          f.user_low=LEAST(sender.id,receiver.id)
                          AND f.user_high=GREATEST(sender.id,receiver.id))
                        AND NOT EXISTS(SELECT 1 FROM user_blocks AS b WHERE
                          (b.blocker_id=sender.id AND b.blocked_id=receiver.id)
                          OR (b.blocker_id=receiver.id AND b.blocked_id=sender.id))
                      RETURNING id
                    )SQL",{me.id,to,label});
                } else {
                    // Apply the same typed membership INSERT as /api/groups/send.
                    created=db.query(R"SQL(
                      INSERT INTO cc_group_messages(group_id,author_id,body)
                      SELECT gm.group_id,gm.user_id,$3::TEXT
                      FROM cc_group_members AS gm
                      WHERE gm.group_id=$1::VARCHAR(24)
                        AND gm.user_id=$2::VARCHAR(32)
                      RETURNING id
                    )SQL",{group,me.id,label});
                }
                if(!PQntuples(created.get()))throw Problem{403,"Recipient permission changed; file not sent."};
                const string msgId=Database::field(created.get(),0,0);
                auto result=db.query(R"SQL(
                  INSERT INTO cc_attachments(dm_message_id,group_message_id,uploader_id,filename,mime,size_bytes,storage_name)
                  VALUES(NULLIF($1::TEXT,'')::BIGINT,NULLIF($2::TEXT,'')::BIGINT,
                         $3::VARCHAR(32),$4::VARCHAR(120),$5::VARCHAR(100),$6::BIGINT,$7::VARCHAR(40))
                  RETURNING id
                )SQL",{scope=="dm"?msgId:"",scope=="group"?msgId:"",me.id,name,mime,std::to_string(total),id+".bin"});
                db.query("DELETE FROM cc_attachment_uploads WHERE upload_id=$1 AND uploader_id=$2",{id,me.id});
                std::filesystem::rename(temp,finalPath);
                try{db.query("COMMIT");}catch(...){std::filesystem::rename(finalPath,temp);throw;}
                if(scope=="dm")events.publish(to);else publishGroup(db,group);
                return "{\"status\":\"sent\",\"attachmentId\":"+Database::field(result.get(),0,0)+"}";
            } catch(...) {
                try{db.query("ROLLBACK");}catch(...){}
                throw;
            }
        }
        // Header-based cursor: bearer token is NEVER put into a URL.
        // A 15-second authenticated long poll wakes as soon as this account
        // has new messages, receipts, typing, or a friend-request change.
        if(r.path=="/api/events"&&r.method=="GET") {
            auto header=r.headers.find("x-cc-cursor");
            if(header==r.headers.end()) {
                return "{\"cursor\":"+std::to_string(events.current(me.id))+
                       ",\"changed\":false}";
            }
            const auto& raw=header->second;
            if(raw.empty()||raw.size()>20||raw.find_first_not_of("0123456789")!=string::npos)
                throw Problem{400,"Invalid notification cursor."};
            std::uint64_t since;
            try { since=std::stoull(raw); }
            catch(...) {throw Problem{400,"Invalid notification cursor."};}
            auto [cursor,hasUpdate]=events.waitFor(me.id,since,std::chrono::seconds(15));
            return "{\"cursor\":"+std::to_string(cursor)+
                   ",\"changed\":"+(hasUpdate?"true":"false")+"}";
        }
        if(r.path=="/api/me"&&r.method=="GET")
            return "{\"id\":"+json(me.id)+",\"name\":"+json(me.name)+",\"publicId\":"+json(me.publicId)+"}";
        if(r.path=="/api/profile" && (r.method=="GET" || r.method=="POST")) {
            if(r.method=="POST") {
                const string displayName=data["display_name"],bio=data["bio"],avatar=data["avatar_theme"];
                if(!validProfileText(displayName,2,48,false))
                    throw Problem{400,"Display name must be 2–48 UTF-8 bytes with no control characters."};
                if(!validProfileText(bio,0,160,true))
                    throw Problem{400,"Bio must be at most 160 UTF-8 bytes."};
                if(!validAvatarTheme(avatar))throw Problem{400,"Choose a supported profile color."};
                // Never let the client set an account ID, public ID, username or friend list.
                db.query("UPDATE users SET display_name=$2,bio=$3,avatar_theme=$4 WHERE id=$1",
                         {me.id,displayName,bio,avatar});
                events.publish(me.id);
                auto friends=db.query(R"SQL(
                    SELECT CASE WHEN user_low=$1 THEN user_high ELSE user_low END
                    FROM friendships WHERE user_low=$1 OR user_high=$1
                )SQL",{me.id});
                for(int i=0;i<PQntuples(friends.get());++i)
                    events.publish(Database::field(friends.get(),i,0));
            }
            auto mine=db.query(R"SQL(
                SELECT COALESCE(NULLIF(display_name,''),username),bio,avatar_theme
                FROM users WHERE id=$1
            )SQL",{me.id});
            return "{\"username\":"+json(me.name)+",\"publicId\":"+json(me.publicId)+
                   ",\"displayName\":"+json(Database::field(mine.get(),0,0))+
                   ",\"bio\":"+json(Database::field(mine.get(),0,1))+
                   ",\"avatarTheme\":"+json(Database::field(mine.get(),0,2))+"}";
        }
        if(r.path=="/api/logout"&&r.method=="POST") {
            store.logout(db,r);std::lock_guard<std::mutex> lock(mutex);typing.erase(me.id);return "{}";
        }
        // Account-managed session revocation: the server always derives the
        // account from the authenticated token, never a client-supplied ID.
        if(r.path=="/api/sessions/revoke_others" && r.method=="POST") {
            const string activeHash=digest(r.headers.at("authorization").substr(7));
            auto removed=db.query("DELETE FROM login_sessions WHERE user_id=$1 AND token_hash<>$2 RETURNING token_hash",
                                  {me.id,activeHash});
            return "{\"status\":\"revoked\",\"count\":"+std::to_string(PQntuples(removed.get()))+"}";
        }
        if(r.path=="/api/sessions/revoke_all" && r.method=="POST") {
            auto removed=db.query("DELETE FROM login_sessions WHERE user_id=$1 RETURNING token_hash",{me.id});
            {std::lock_guard<std::mutex> lock(mutex);typing.erase(me.id);}
            return "{\"status\":\"revoked\",\"count\":"+std::to_string(PQntuples(removed.get()))+"}";
        }
        // Account-level safety controls are authorized by the active login,
        // never by a user ID supplied as an owner identifier.
        if((r.path=="/api/friends/block" || r.path=="/api/friends/unblock" ||
            r.path=="/api/friends/remove" || r.path=="/api/friends/mute" ||
            r.path=="/api/friends/unmute") && r.method=="POST") {
            const string wanted=normalizedPublicId(data["public_id"]);
            if(wanted==me.publicId)throw Problem{400,"Cannot change your own friend settings."};
            auto result=db.query("SELECT id FROM users WHERE public_id=$1",{wanted});
            if(PQntuples(result.get())==0)throw Problem{404,"Account not found."};
            const string peer=Database::field(result.get(),0,0);
            const string action=r.path;
            if(action=="/api/friends/block") {
                // Blocks persist; delete accepted/pending connections in both directions.
                // Preserve historical messages in the database, but they are no longer
                // accessible without a new, accepted friendship after unblock.
                db.query("BEGIN");
                db.query("INSERT INTO user_blocks(blocker_id,blocked_id) VALUES($1,$2) "
                         "ON CONFLICT DO NOTHING",{me.id,peer});
                db.query("DELETE FROM friendships WHERE user_low=LEAST($1,$2) AND user_high=GREATEST($1,$2)",
                         {me.id,peer});
                db.query("DELETE FROM friend_requests WHERE (sender_id=$1 AND receiver_id=$2) "
                         "OR (sender_id=$2 AND receiver_id=$1)",{me.id,peer});
                db.query("DELETE FROM muted_contacts WHERE (owner_id=$1 AND peer_id=$2) "
                         "OR (owner_id=$2 AND peer_id=$1)",{me.id,peer});
                db.query("COMMIT");
                events.publish(peer);events.publish(me.id);
                return "{\"status\":\"blocked\"}";
            }
            if(action=="/api/friends/unblock") {
                auto changed=db.query("DELETE FROM user_blocks WHERE blocker_id=$1 AND blocked_id=$2 "
                                      "RETURNING blocked_id",{me.id,peer});
                if(PQntuples(changed.get())==0)throw Problem{404,"Account is not on your block list."};
                events.publish(me.id);
                return "{\"status\":\"unblocked\"}";
            }
            if(action=="/api/friends/remove") {
                auto changed=db.query("DELETE FROM friendships WHERE user_low=LEAST($1,$2) "
                                      "AND user_high=GREATEST($1,$2) RETURNING user_low",{me.id,peer});
                if(PQntuples(changed.get())==0)throw Problem{404,"You are no longer connected with this friend."};
                db.query("DELETE FROM muted_contacts WHERE (owner_id=$1 AND peer_id=$2) "
                         "OR (owner_id=$2 AND peer_id=$1)",{me.id,peer});
                events.publish(peer);events.publish(me.id);
                return "{\"status\":\"removed\"}";
            }
            if(!store.areFriends(db,me.id,peer))throw Problem{403,"Only connected friends can be muted."};
            if(action=="/api/friends/mute")
                db.query("INSERT INTO muted_contacts(owner_id,peer_id) VALUES($1,$2) "
                         "ON CONFLICT DO NOTHING",{me.id,peer});
            else
                db.query("DELETE FROM muted_contacts WHERE owner_id=$1 AND peer_id=$2",{me.id,peer});
            events.publish(me.id);
            return "{\"status\":"+json(action=="/api/friends/mute"?"muted":"unmuted")+"}";
        }
        if(r.path=="/api/connect"&&r.method=="POST") {
            const string wanted=normalizedPublicId(data["public_id"]);
            if(wanted==me.publicId)throw Problem{400,"You cannot connect to yourself."};
            auto receiver=db.query("SELECT id,username FROM users WHERE public_id=$1",{wanted});
            if(PQntuples(receiver.get())==0)throw Problem{404,"No account has that ChatConnect ID. Check it with your friend."};
            const string receiverId=Database::field(receiver.get(),0,0);
            const string receiverName=Database::field(receiver.get(),0,1);
            if(store.isBlockedEitherWay(db,me.id,receiverId))
                throw Problem{403,"Connection unavailable. Check with the other person."};
            if(store.areFriends(db,me.id,receiverId))
                return "{\"status\":\"connected\",\"name\":"+json(receiverName)+"}";
            auto pending=db.query("SELECT sender_id FROM friend_requests WHERE status='pending' AND "
                                  "LEAST(sender_id,receiver_id)=LEAST($1,$2) AND "
                                  "GREATEST(sender_id,receiver_id)=GREATEST($1,$2) LIMIT 1",{me.id,receiverId});
            if(PQntuples(pending.get())!=0) {
                const bool incoming=Database::field(pending.get(),0,0)!=me.id;
                return "{\"status\":"+json(incoming?"incoming":"pending")+",\"name\":"+json(receiverName)+"}";
            }
            db.query("INSERT INTO friend_requests(sender_id,receiver_id,status) VALUES($1,$2,'pending')",
                     {me.id,receiverId});
            events.publish(receiverId);
            return "{\"status\":\"pending\",\"name\":"+json(receiverName)+"}";
        }
        if((r.path=="/api/requests/accept"||r.path=="/api/requests/reject")&&r.method=="POST") {
            const auto id=data["request_id"];
            if(id.empty()||id.size()>19||id.find_first_not_of("0123456789")!=string::npos)
                throw Problem{400,"Invalid friend request."};
            const bool accept=r.path=="/api/requests/accept";
            if(accept) {
                auto requester=db.query("SELECT sender_id FROM friend_requests "
                                        "WHERE id=$1 AND receiver_id=$2 AND status='pending'",{id,me.id});
                if(PQntuples(requester.get())==0)throw Problem{404,"Pending friend request not found."};
                if(store.isBlockedEitherWay(db,me.id,Database::field(requester.get(),0,0)))
                    throw Problem{403,"Connection unavailable."};
            }
            db.query("BEGIN");
            auto rows=db.query("UPDATE friend_requests SET status=$1,responded_at=now() "
                               "WHERE id=$2 AND receiver_id=$3 AND status='pending' "
                               "RETURNING sender_id,receiver_id",
                               {accept?"accepted":"rejected",id,me.id});
            if(PQntuples(rows.get())==0)throw Problem{404,"Pending friend request not found."};
            if(accept) {
                const auto sender=Database::field(rows.get(),0,0);
                const auto receiver=Database::field(rows.get(),0,1);
                db.query("INSERT INTO friendships(user_low,user_high) VALUES(LEAST($1,$2),GREATEST($1,$2)) "
                         "ON CONFLICT (user_low,user_high) DO NOTHING",{sender,receiver});
            }
            const string requester=Database::field(rows.get(),0,0);
            db.query("COMMIT");
            events.publish(requester);
            events.publish(me.id);
            return "{\"status\":"+json(accept?"connected":"rejected")+"}";
        }
        // Group chats: membership is a server-side PostgreSQL decision,
        // never a client-provided user list. Invitations require approval.
        if(r.path=="/api/groups/create" && r.method=="POST") {
            const string title=data["name"];
            if(!validProfileText(title,3,64,false))
                throw Problem{400,"Group name must be 3–64 UTF-8 bytes."};
            const string id=secureRandom(12);
            db.query("BEGIN");
            db.query("INSERT INTO cc_groups(id,name,owner_id) VALUES($1,$2,$3)",{id,title,me.id});
            db.query("INSERT INTO cc_group_members(group_id,user_id,role) VALUES($1,$2,'owner')",{id,me.id});
            db.query("COMMIT");
            events.publish(me.id);
            return "{\"groupId\":"+json(id)+"}";
        }
        if(r.path=="/api/groups/invite" && r.method=="POST") {
            const string group=data["group_id"];
            if(!validGroupId(group))throw Problem{400,"Invalid group ID."};
            if(!isGroupOwner(db,group,me.id))throw Problem{403,"Only the group owner can invite friends."};
            const string wanted=normalizedPublicId(data["public_id"]);
            auto person=db.query("SELECT id FROM users WHERE public_id=$1",{wanted});
            if(PQntuples(person.get())==0)throw Problem{404,"Friend not found."};
            const string friendId=Database::field(person.get(),0,0);
            if(friendId==me.id||!store.areFriends(db,me.id,friendId))
                throw Problem{403,"Only your accepted friends can be invited."};
            if(isGroupMember(db,group,friendId))throw Problem{409,"Friend is already a member."};
            auto pending=db.query("SELECT 1 FROM cc_group_invites WHERE group_id=$1 AND invitee_id=$2 AND status='pending'",{group,friendId});
            if(PQntuples(pending.get()))return "{\"status\":\"pending\"}";
            db.query("INSERT INTO cc_group_invites(group_id,inviter_id,invitee_id) VALUES($1,$2,$3)",{group,me.id,friendId});
            events.publish(friendId);
            return "{\"status\":\"pending\"}";
        }
        if(r.path=="/api/groups/respond" && r.method=="POST") {
            const string invite=data["invite_id"],decision=data["decision"];
            if(!validSqlNumber(invite)||(decision!="accept"&&decision!="decline"))
                throw Problem{400,"Invalid group invitation response."};
            auto row=db.query(R"SQL(
                SELECT i.group_id,i.inviter_id
                FROM cc_group_invites i
                JOIN cc_groups g ON g.id=i.group_id
                WHERE i.id=$1::BIGINT AND i.invitee_id=$2 AND i.status='pending'
            )SQL",{invite,me.id});
            if(!PQntuples(row.get()))throw Problem{404,"Pending group invitation not found."};
            const string group=Database::field(row.get(),0,0),inviter=Database::field(row.get(),0,1);
            if(decision=="accept" && (!isGroupOwner(db,group,inviter)||
                                      !store.areFriends(db,me.id,inviter)))
                throw Problem{403,"This group invite is no longer valid."};
            db.query("BEGIN");
            auto updated=db.query("UPDATE cc_group_invites SET status=$3,answered_at=now() "
                        "WHERE id=$1::BIGINT AND invitee_id=$2 AND status='pending' RETURNING group_id",
                        {invite,me.id,decision=="accept"?"accepted":"declined"});
            if(PQntuples(updated.get())!=1)throw Problem{409,"Invitation already handled."};
            if(decision=="accept")db.query("INSERT INTO cc_group_members(group_id,user_id,role) "
                        "VALUES($1,$2,'member') ON CONFLICT DO NOTHING",{group,me.id});
            db.query("COMMIT");
            publishGroup(db,group); events.publish(me.id);
            return "{\"status\":"+json(decision=="accept"?"joined":"declined")+"}";
        }
        // Group management: only the owner can rename a group.
        if(r.path=="/api/groups/rename" && r.method=="POST") {
            const string group=data["group_id"],title=data["name"];
            if(!validGroupId(group)||!validProfileText(title,3,64,false))
                throw Problem{400,"Group name must be 3-64 UTF-8 bytes."};
            auto changed=db.query("UPDATE cc_groups SET name=$3 "
                "WHERE id=$1 AND owner_id=$2 RETURNING id",{group,me.id,title});
            if(!PQntuples(changed.get()))throw Problem{403,"Only group owner can rename."};
            publishGroup(db,group);
            return "{\"status\":\"renamed\"}";
        }
        // The owner edits group metadata. Only current group members receive
        // description or photo via /api/state; never publish it to strangers.
        if(r.path=="/api/groups/description" && r.method=="POST") {
            const string group=data["group_id"],description=data["description"];
            if(!validGroupId(group)||!validProfileText(description,0,240,true))
                throw Problem{400,"Description must be at most 240 bytes."};
            auto changed=db.query("UPDATE cc_groups SET description=$3 WHERE id=$1 AND owner_id=$2 RETURNING id",
                {group,me.id,description});
            if(!PQntuples(changed.get()))throw Problem{403,"Only group owner can edit its description."};
            publishGroup(db,group);
            return "{\"status\":\"updated\"}";
        }
        if(r.path=="/api/groups/photo" && r.method=="POST") {
            const string group=data["group_id"],photo=data["photo_data"];
            if(!validGroupId(group)||(!photo.empty()&&!validGroupPhoto(photo)))
                throw Problem{400,"Group photo must be a valid JPEG below 45 KB."};
            auto changed=db.query("UPDATE cc_groups SET photo_data=$3 WHERE id=$1 AND owner_id=$2 RETURNING id",
                {group,me.id,photo});
            if(!PQntuples(changed.get()))throw Problem{403,"Only group owner can change its photo."};
            publishGroup(db,group);
            return "{\"status\":\"updated\"}";
        }
        // Mute belongs to one member. It must not affect anybody's unread badge.
        if(r.path=="/api/groups/mute" && r.method=="POST") {
            const string group=data["group_id"],muted=data["muted"];
            if(!validGroupId(group)||(muted!="1"&&muted!="0"))throw Problem{400,"Invalid group mute preference."};
            if(!isGroupMember(db,group,me.id))throw Problem{403,"Not a group member."};
            if(muted=="1")db.query("INSERT INTO cc_group_mutes(group_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",{group,me.id});
            else db.query("DELETE FROM cc_group_mutes WHERE group_id=$1 AND user_id=$2",{group,me.id});
            events.publish(me.id);
            return "{\"status\":\"updated\"}";
        }
        // Group message controls: membership and authorship are both checked
        // server-side. Owner can moderate (delete), but cannot edit others.
        if ((r.path=="/api/groups/edit" || r.path=="/api/groups/delete-message" ||
             r.path=="/api/groups/delete-me") && r.method=="POST") {
            const string group=data["group_id"],id=data["id"];
            if(!validGroupId(group)||!validSqlNumber(id))
                throw Problem{400,"Invalid group message reference."};
            if(!isGroupMember(db,group,me.id))
                throw Problem{403,"Only current group members can modify their own view."};
            auto found=db.query(R"SQL(
                SELECT m.author_id,m.deleted_at IS NOT NULL
                FROM cc_group_messages m
                JOIN cc_group_members gm
                  ON gm.group_id=m.group_id AND gm.user_id=$3
                WHERE m.id=$1::BIGINT AND m.group_id=$2
                  AND m.created_at>=gm.joined_at
            )SQL",{id,group,me.id});
            if(!PQntuples(found.get()))throw Problem{404,"Group message not found."};
            if(r.path=="/api/groups/delete-me") {
                db.query(R"SQL(
                    INSERT INTO cc_group_message_hides(message_id,user_id)
                    VALUES($1::BIGINT,$2) ON CONFLICT DO NOTHING
                )SQL",{id,me.id});
                events.publish(me.id);
                return "{\"status\":\"deleted_for_me\"}";
            }
            const string author=Database::field(found.get(),0,0);
            const bool editing=r.path=="/api/groups/edit";
            if(editing && author!=me.id)
                throw Problem{403,"Only the author can edit a group message."};
            if(editing) {
                auto file=db.query("SELECT 1 FROM cc_attachments WHERE group_message_id=$1::BIGINT",{id});
                if(PQntuples(file.get()))throw Problem{409,"Attachments cannot be edited; delete and resend instead."};
            }
            if(!editing && author!=me.id && !isGroupOwner(db,group,me.id))
                throw Problem{403,"Only the author or group owner can delete for everyone."};
            if(Database::field(found.get(),0,1)=="t")
                throw Problem{409,"This message was already deleted."};
            string body=data["text"];
            if(editing && (body.empty()||body.size()>2000||
               body.find_first_not_of(" \t\n\r")==string::npos))
                throw Problem{400,"Write a message up to 2000 bytes."};
            Database::Result changed=editing ? db.query(R"SQL(
                UPDATE cc_group_messages m SET body=$4,edited_at=now()
                WHERE m.id=$1::BIGINT AND m.group_id=$2 AND m.author_id=$3
                  AND m.deleted_at IS NULL
                  AND EXISTS(SELECT 1 FROM cc_group_members gm WHERE gm.group_id=m.group_id AND gm.user_id=$3)
                RETURNING m.id
            )SQL",{id,group,me.id,body}) : db.query(R"SQL(
                UPDATE cc_group_messages m SET body='',deleted_at=now()
                WHERE m.id=$1::BIGINT AND m.group_id=$2 AND m.deleted_at IS NULL
                  AND EXISTS(SELECT 1 FROM cc_group_members gm WHERE gm.group_id=m.group_id AND gm.user_id=$3)
                  AND (m.author_id=$3 OR EXISTS(SELECT 1 FROM cc_groups g WHERE g.id=m.group_id AND g.owner_id=$3))
                RETURNING m.id
            )SQL",{id,group,me.id});
            if(!PQntuples(changed.get()))throw Problem{409,"Message changed or membership revoked."};
            if(!editing)db.query("DELETE FROM cc_attachments WHERE group_message_id=$1::BIGINT",{id});
            publishGroup(db,group);
            return editing ? "{\"status\":\"edited\"}" : "{\"status\":\"deleted\"}";
        }
        if(r.path=="/api/groups/react" && r.method=="POST") {
            const string group=data["group_id"], id=data["id"], emoji=data["emoji"];
            if(!validGroupId(group)||!validSqlNumber(id)||(!emoji.empty()&&!validReactionEmoji(emoji)))
                throw Problem{400,"Invalid group reaction."};
            const auto visible=db.query(R"SQL(
                SELECT 1 FROM cc_group_messages m
                JOIN cc_group_members gm ON gm.group_id=m.group_id AND gm.user_id=$1::VARCHAR(32)
                WHERE m.id=$2::BIGINT AND m.group_id=$3::VARCHAR(24)
                  AND m.deleted_at IS NULL AND m.created_at>=gm.joined_at
                  AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                                 WHERE h.message_id=m.id AND h.user_id=$1::VARCHAR(32))
                  AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                 WHERE (b.blocker_id=$1::VARCHAR(32) AND b.blocked_id=m.author_id)
                                    OR (b.blocker_id=m.author_id AND b.blocked_id=$1::VARCHAR(32)))
            )SQL",{me.id,id,group});
            if(!PQntuples(visible.get()))throw Problem{404,"Group message unavailable."};
            if(emoji.empty())db.query("DELETE FROM cc_group_reactions WHERE message_id=$1::BIGINT AND user_id=$2::VARCHAR(32)",{id,me.id});
            else db.query(R"SQL(
                INSERT INTO cc_group_reactions(message_id,user_id,emoji)
                VALUES ($1::BIGINT,$2::VARCHAR(32),$3::TEXT)
                ON CONFLICT(message_id,user_id) DO UPDATE SET emoji=EXCLUDED.emoji
            )SQL",{id,me.id,emoji});
            publishGroup(db,group);
            return "{\"status\":\"updated\"}";
        }
        if(r.path=="/api/groups/send" && r.method=="POST") {
            const string group=data["group_id"],body=data["text"],reply=data["reply_to"],clientRef=data["client_ref"];
            if(!clientRef.empty() && !validClientMessageRef(clientRef))
                throw Problem{400,"Invalid client message reference."};
            if(!validGroupId(group))throw Problem{400,"Invalid group ID."};
            if(!reply.empty() && !validSqlNumber(reply))throw Problem{400,"Invalid reply message ID."};
            if(body.empty()||body.size()>2000||body.find_first_not_of(" \t\n\r")==string::npos)
                throw Problem{400,"Write a message up to 2000 bytes."};
            // Only members can send. Also enforce membership inside INSERT to avoid races.
            if(!isGroupMember(db,group,me.id))
                throw Problem{403,"Only group members can send messages."};
            if(!reply.empty()) {
                auto original=db.query(R"SQL(
                    SELECT 1 FROM cc_group_messages m
                    JOIN cc_group_members gm ON gm.group_id=m.group_id AND gm.user_id=$1::VARCHAR(32)
                    WHERE m.id=$2::BIGINT AND m.group_id=$3::VARCHAR(24)
                      AND m.deleted_at IS NULL AND m.created_at>=gm.joined_at
                      AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                                     WHERE h.message_id=m.id AND h.user_id=$1::VARCHAR(32))
                      AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                     WHERE (b.blocker_id=$1::VARCHAR(32) AND b.blocked_id=m.author_id)
                                        OR (b.blocker_id=m.author_id AND b.blocked_id=$1::VARCHAR(32)))
                )SQL",{me.id,reply,group});
                if(!PQntuples(original.get()))throw Problem{403,"Cannot reply to a message outside this group."};
            }
            // Legacy clients still work without a client_ref. New clients can retry safely.
            if(clientRef.empty()) {
                auto written=db.query(R"SQL(
                    INSERT INTO cc_group_messages(group_id,author_id,body,reply_to_id)
                    SELECT gm.group_id,gm.user_id,$3::TEXT,NULLIF($4::TEXT,'')::BIGINT
                    FROM cc_group_members AS gm
                    WHERE gm.group_id=$1::VARCHAR(24) AND gm.user_id=$2::VARCHAR(32)
                    RETURNING id
                )SQL",{group,me.id,body,reply});
                if(!PQntuples(written.get()))throw Problem{403,"Only group members can send messages."};
                publishGroup(db,group);
                return "{\"id\":"+Database::field(written.get(),0,0)+"}";
            }
            const string fingerprint=digest("group\n"+group+"\n"+body+"\n"+reply);
            auto written=db.query(R"SQL(
                INSERT INTO cc_group_messages(group_id,author_id,body,reply_to_id,client_ref,client_payload_hash)
                SELECT gm.group_id,gm.user_id,$3::TEXT,NULLIF($4::TEXT,'')::BIGINT,
                       $5::VARCHAR(32),$6::VARCHAR(64)
                FROM cc_group_members AS gm
                WHERE gm.group_id=$1::VARCHAR(24) AND gm.user_id=$2::VARCHAR(32)
                ON CONFLICT (author_id,client_ref) WHERE client_ref IS NOT NULL DO NOTHING
                RETURNING id
            )SQL",{group,me.id,body,reply,clientRef,fingerprint});
            if(PQntuples(written.get())) {
                publishGroup(db,group);
                return "{\"id\":"+Database::field(written.get(),0,0)+",\"deduplicated\":false}";
            }
            auto existing=db.query(R"SQL(
                SELECT id::TEXT,client_payload_hash FROM cc_group_messages
                WHERE author_id=$1::VARCHAR(32) AND client_ref=$2::VARCHAR(32)
            )SQL",{me.id,clientRef});
            if(!PQntuples(existing.get()))throw Problem{403,"Group membership changed; message not saved."};
            if(Database::field(existing.get(),0,1)!=fingerprint)
                throw Problem{409,"Message reference already used for different content."};
            return "{\"id\":"+Database::field(existing.get(),0,0)+",\"deduplicated\":true}";
        }
        if(r.path=="/api/groups/read" && r.method=="POST") {
            const string group=data["group_id"],upTo=data["up_to"];
            if(!validGroupId(group)||!validSqlNumber(upTo))
                throw Problem{400,"Invalid group read marker."};
            if(!isGroupMember(db,group,me.id))throw Problem{403,"Not a group member."};
            auto maxSeen=db.query(R"SQL(
                SELECT COALESCE(MAX(m.id),0)::TEXT
                FROM cc_group_messages m JOIN cc_group_members mem
                  ON mem.group_id=m.group_id AND mem.user_id=$2
                WHERE m.group_id=$1 AND m.id<=$3::BIGINT AND m.created_at>=mem.joined_at
            )SQL",{group,me.id,upTo});
            const auto verified=Database::field(maxSeen.get(),0,0);
            if(verified!="0"&&!verified.empty())db.query(R"SQL(
                INSERT INTO cc_group_read_markers(group_id,reader_id,last_read_id)
                VALUES($1,$2,$3::BIGINT)
                ON CONFLICT(group_id,reader_id) DO UPDATE SET
                last_read_id=GREATEST(cc_group_read_markers.last_read_id,EXCLUDED.last_read_id)
            )SQL",{group,me.id,verified});
            events.publish(me.id);
            return "{\"status\":\"read\"}";
        }
        if(r.path=="/api/groups/remove" && r.method=="POST") {
            const string group=data["group_id"],member=data["member_id"];
            if(!validGroupId(group)||member.empty()||member.size()>32)
                throw Problem{400,"Invalid group member."};
            if(!isGroupOwner(db,group,me.id))throw Problem{403,"Only group owner can remove members."};
            if(member==me.id)throw Problem{403,"Owner cannot remove themselves. Delete the group instead."};
            auto changed=db.query("DELETE FROM cc_group_members WHERE group_id=$1 AND user_id=$2 RETURNING user_id",{group,member});
            if(!PQntuples(changed.get()))throw Problem{404,"Member not found."};
            events.publish(member);publishGroup(db,group);
            return "{\"status\":\"removed\"}";
        }
        if(r.path=="/api/groups/leave" && r.method=="POST") {
            const string group=data["group_id"];
            if(!validGroupId(group))throw Problem{400,"Invalid group ID."};
            if(isGroupOwner(db,group,me.id))throw Problem{403,"Owner must delete group instead of leaving."};
            auto deleted=db.query("DELETE FROM cc_group_members WHERE group_id=$1 AND user_id=$2 RETURNING user_id",{group,me.id});
            if(!PQntuples(deleted.get()))throw Problem{403,"Not a group member."};
            publishGroup(db,group);events.publish(me.id);
            return "{\"status\":\"left\"}";
        }
        if(r.path=="/api/groups/delete" && r.method=="POST") {
            const string group=data["group_id"];
            if(!validGroupId(group))throw Problem{400,"Invalid group ID."};
            if(!isGroupOwner(db,group,me.id))throw Problem{403,"Only group owner can delete this group."};
            auto rows=db.query("SELECT user_id FROM cc_group_members WHERE group_id=$1",{group});
            auto invites=db.query("SELECT invitee_id FROM cc_group_invites WHERE group_id=$1 AND status='pending'",{group});
            db.query("DELETE FROM cc_groups WHERE id=$1 AND owner_id=$2",{group,me.id});
            for(int i=0;i<PQntuples(rows.get());++i)events.publish(Database::field(rows.get(),i,0));
            for(int i=0;i<PQntuples(invites.get());++i)events.publish(Database::field(invites.get(),i,0));
            return "{\"status\":\"deleted\"}";
        }
        // Server-side search covers messages older than /api/state's 300 DM / 500 group window.
        // Reuse the SAME friendship, member, block, join-time and Delete-for-Me visibility
        // predicates as /api/state. No unauthenticated search or public message indexing.
        if(r.path=="/api/search/messages" && r.method=="POST") {
            const string term=data["query"], context=data["context"], thread=data["thread_id"];
            if(term.size()<2 || term.size()>160 ||
               term.find_first_not_of(" \t\r\n")==string::npos ||
               !validProfileText(term,2,160,false))
                throw Problem{400,"Enter 2–160 characters to search."};
            if(context!="all" && context!="friend" && context!="group" && context!="notes")
                throw Problem{400,"Invalid search scope."};
            if((context=="friend" && (thread.empty() || thread.size()>32 ||
                thread.find_first_not_of("0123456789abcdef")!=string::npos)) ||
               (context=="group" && !validGroupId(thread)))
                throw Problem{400,"Invalid conversation."};
            struct Hit {long long time; long long id; string payload;};
            std::vector<Hit> hits;
            if(context!="group") {
                auto rows=db.query(R"SQL(
                    SELECT m.id::TEXT,m.author_id,
                           COALESCE(NULLIF(u.display_name,''),u.username),
                           COALESCE(m.recipient_id,''),
                           EXTRACT(EPOCH FROM m.created_at)::BIGINT::TEXT,
                           m.body,COALESCE(a.filename,''),
                           COALESCE(a.id::TEXT,''),COALESCE(a.mime,''),COALESCE(a.size_bytes::TEXT,'0'),
                           CASE WHEN m.recipient_id IS NULL THEN 'notes' ELSE 'friend' END,
                           CASE WHEN m.recipient_id IS NULL THEN ''
                                WHEN m.author_id=$1::VARCHAR(32) THEN m.recipient_id ELSE m.author_id END,
                           CASE WHEN m.recipient_id IS NULL THEN 'My private notes'
                                ELSE COALESCE(NULLIF(peer.display_name,''),peer.username) END
                    FROM messages m
                    JOIN users u ON u.id=m.author_id
                    LEFT JOIN users peer ON peer.id=CASE
                       WHEN m.author_id=$1::VARCHAR(32) THEN m.recipient_id ELSE m.author_id END
                    LEFT JOIN cc_attachments a ON a.dm_message_id=m.id
                    WHERE m.deleted_at IS NULL
                      AND NOT EXISTS(SELECT 1 FROM message_hidden_for_user h
                                     WHERE h.user_id=$1::VARCHAR(32) AND h.message_id=m.id)
                      AND (
                        (m.author_id=$1::VARCHAR(32) AND m.recipient_id IS NULL)
                        OR (m.recipient_id IS NOT NULL
                            AND (m.author_id=$1::VARCHAR(32) OR m.recipient_id=$1::VARCHAR(32))
                            AND EXISTS(SELECT 1 FROM friendships f
                                       WHERE f.user_low=LEAST(m.author_id,m.recipient_id)
                                         AND f.user_high=GREATEST(m.author_id,m.recipient_id)
                                         AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                             WHERE (b.blocker_id=m.author_id AND b.blocked_id=m.recipient_id)
                                                OR (b.blocker_id=m.recipient_id AND b.blocked_id=m.author_id)))))
                      AND (strpos(lower(m.body),lower($2::TEXT))>0
                           OR strpos(lower(COALESCE(a.filename,'')),lower($2::TEXT))>0)
                      AND ($3::TEXT='all' OR
                           ($3::TEXT='notes' AND m.recipient_id IS NULL) OR
                           ($3::TEXT='friend' AND m.recipient_id IS NOT NULL AND
                               (CASE WHEN m.author_id=$1::VARCHAR(32) THEN m.recipient_id
                                     ELSE m.author_id END)=$4::VARCHAR(32)))
                    ORDER BY m.created_at DESC,m.id DESC LIMIT 70
                )SQL",{me.id,term,context,thread});
                for(int i=0;i<PQntuples(rows.get());++i){
                    const auto f=[&](int col){return Database::field(rows.get(),i,col);};
                    const string attach=f(7).empty()?"null":
                        "{\"id\":"+f(7)+",\"name\":"+json(f(6))+
                        ",\"mime\":"+json(f(8))+
                        ",\"size\":"+f(9)+"}";
                    string payload="{\"kind\":"+json(f(10))+
                        ",\"threadId\":"+json(f(11))+
                        ",\"threadName\":"+json(f(12))+
                        ",\"id\":"+json(f(0))+
                        ",\"author\":"+json(f(1))+
                        ",\"authorName\":"+json(f(2))+
                        ",\"to\":"+json(f(3))+
                        ",\"time\":"+f(4)+
                        ",\"text\":"+json(f(5))+
                        ",\"attachment\":"+attach+"}";
                    hits.push_back({std::stoll(f(4)),std::stoll(f(0)),std::move(payload)});
                }
            }
            if(context=="all" || context=="group") {
                auto rows=db.query(R"SQL(
                    SELECT m.id::TEXT,m.group_id,g.name,m.author_id,
                           COALESCE(NULLIF(u.display_name,''),u.username),
                           EXTRACT(EPOCH FROM m.created_at)::BIGINT::TEXT,m.body,
                           COALESCE(a.filename,''),COALESCE(a.id::TEXT,''),
                           COALESCE(a.mime,''),COALESCE(a.size_bytes::TEXT,'0')
                    FROM cc_group_messages m
                    JOIN cc_group_members gm ON gm.group_id=m.group_id AND gm.user_id=$1::VARCHAR(32)
                    JOIN cc_groups g ON g.id=m.group_id
                    JOIN users u ON u.id=m.author_id
                    LEFT JOIN cc_attachments a ON a.group_message_id=m.id
                    WHERE m.created_at>=gm.joined_at AND m.deleted_at IS NULL
                      AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                                     WHERE h.message_id=m.id AND h.user_id=$1::VARCHAR(32))
                      AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                     WHERE (b.blocker_id=$1::VARCHAR(32) AND b.blocked_id=m.author_id)
                                        OR (b.blocker_id=m.author_id AND b.blocked_id=$1::VARCHAR(32)))
                      AND (strpos(lower(m.body),lower($2::TEXT))>0
                           OR strpos(lower(COALESCE(a.filename,'')),lower($2::TEXT))>0)
                      AND ($3::TEXT='all' OR m.group_id=$4::VARCHAR(24))
                    ORDER BY m.created_at DESC,m.id DESC LIMIT 70
                )SQL",{me.id,term,context,thread});
                for(int i=0;i<PQntuples(rows.get());++i){
                    const auto f=[&](int col){return Database::field(rows.get(),i,col);};
                    const string attach=f(8).empty()?"null":
                        "{\"id\":"+f(8)+",\"name\":"+json(f(7))+
                        ",\"mime\":"+json(f(9))+
                        ",\"size\":"+f(10)+"}";
                    string payload="{\"kind\":\"group\",\"threadId\":"+json(f(1))+
                        ",\"threadName\":"+json(f(2))+
                        ",\"id\":"+json(f(0))+
                        ",\"author\":"+json(f(3))+
                        ",\"authorName\":"+json(f(4))+
                        ",\"to\":\"\",\"time\":"+f(5)+
                        ",\"text\":"+json(f(6))+
                        ",\"attachment\":"+attach+"}";
                    hits.push_back({std::stoll(f(5)),std::stoll(f(0)),std::move(payload)});
                }
            }
            std::sort(hits.begin(),hits.end(),[](const Hit& a,const Hit& b){
                return a.time!=b.time?a.time>b.time:a.id>b.id;
            });
            const bool truncated=hits.size()>60;
            string result="{\"results\":[";
            for(size_t i=0;i<std::min(size_t(60),hits.size());++i){
                if(i)result+=",";
                result+=hits[i].payload;
            }
            result+="],\"truncated\":";
            result+=truncated?"true":"false";
            result+="}";
            return result;
        }
        // CC_MEDIA_GALLERY_V1: paginated metadata for attachments sent by this signed-in account.
        // This endpoint NEVER returns file contents or public URLs. Existing attachment
        // preview/download handlers re-check message authorization before serving bytes.
        if(r.path=="/api/media/mine" && r.method=="POST") {
            const string scope=data["scope"],thread=data["thread_id"],kind=data["kind"];
            const string before=data["before"];
            if(scope!="notes" && scope!="friend" && scope!="group")
                throw Problem{400,"Invalid media conversation."};
            if(kind!="all" && kind!="photos" && kind!="files")
                throw Problem{400,"Invalid media filter."};
            if(!before.empty() && (before.size()>19 || !validSqlNumber(before)))
                throw Problem{400,"Invalid media page cursor."};
            if(scope=="notes" && !thread.empty())throw Problem{400,"Invalid notes context."};
            if(scope=="friend") {
                if(thread.empty() || thread.size()>32 ||
                   thread.find_first_not_of("0123456789abcdef")!=string::npos ||
                   thread==me.id || !store.areFriends(db,me.id,thread))
                    throw Problem{403,"Only connected friends' chats can be viewed."};
            }
            if(scope=="group" && (!validGroupId(thread)||!isGroupMember(db,thread,me.id)))
                throw Problem{403,"Group membership required."};
            const string cursor=before.empty()?"9223372036854775807":before;
            auto rows=(scope=="group")?db.query(R"SQL(
                SELECT a.id::TEXT,a.filename,a.mime,a.size_bytes::TEXT,
                       EXTRACT(EPOCH FROM m.created_at)::BIGINT::TEXT,m.id::TEXT
                FROM cc_attachments a
                JOIN cc_group_messages m ON m.id=a.group_message_id
                JOIN cc_group_members gm ON gm.group_id=m.group_id
                                        AND gm.user_id=$1::VARCHAR(32)
                WHERE m.group_id=$2::VARCHAR(24)
                  AND m.author_id=$1::VARCHAR(32) AND a.uploader_id=$1::VARCHAR(32)
                  AND m.deleted_at IS NULL AND m.created_at>=gm.joined_at
                  AND m.id<$3::BIGINT
                  AND NOT EXISTS (SELECT 1 FROM cc_group_message_hides h
                                  WHERE h.message_id=m.id AND h.user_id=$1::VARCHAR(32))
                  AND a.mime NOT LIKE 'audio/%'
                  AND ($4::TEXT='all' OR
                      ($4::TEXT='photos' AND a.mime LIKE 'image/%') OR
                      ($4::TEXT='files' AND a.mime NOT LIKE 'image/%'))
                ORDER BY m.id DESC LIMIT 31
            )SQL",{me.id,thread,cursor,kind}):db.query(R"SQL(
                SELECT a.id::TEXT,a.filename,a.mime,a.size_bytes::TEXT,
                       EXTRACT(EPOCH FROM m.created_at)::BIGINT::TEXT,m.id::TEXT
                FROM cc_attachments a
                JOIN messages m ON m.id=a.dm_message_id
                WHERE m.author_id=$1::VARCHAR(32) AND a.uploader_id=$1::VARCHAR(32)
                  AND m.deleted_at IS NULL AND m.id<$3::BIGINT
                  AND NOT EXISTS (SELECT 1 FROM message_hidden_for_user h
                                  WHERE h.message_id=m.id AND h.user_id=$1::VARCHAR(32))
                  AND (($2::TEXT='notes' AND m.recipient_id IS NULL) OR
                       ($2::TEXT='friend' AND m.recipient_id=$5::VARCHAR(32)))
                  AND a.mime NOT LIKE 'audio/%'
                  AND ($4::TEXT='all' OR
                      ($4::TEXT='photos' AND a.mime LIKE 'image/%') OR
                      ($4::TEXT='files' AND a.mime NOT LIKE 'image/%'))
                ORDER BY m.id DESC LIMIT 31
            )SQL",{me.id,scope,cursor,kind,thread});
            const int count=PQntuples(rows.get());
            const int shown=std::min(count,30);
            string response="{\"items\":[";
            for(int i=0;i<shown;++i){
                if(i)response+=",";
                const auto f=[&](int col){return Database::field(rows.get(),i,col);};
                response+="{\"id\":"+json(f(0))+
                    ",\"name\":"+json(f(1))+
                    ",\"mime\":"+json(f(2))+
                    ",\"size\":"+f(3)+
                    ",\"time\":"+f(4)+
                    ",\"messageId\":"+json(f(5))+"}";
            }
            response+="],\"has_more\":";
            response+=(count>30?"true":"false");
            response+=",\"next_before\":";
            response+=shown?json(Database::field(rows.get(),shown-1,5)):"null";
            response+="}";
            return response;
        }

        if(r.path=="/api/state"&&r.method=="GET") {
            db.query("UPDATE users SET last_seen=now() WHERE id=$1",{me.id});
            // Only mutual friends are listed; strangers cannot browse the user directory.
            auto people=db.query(R"SQL(
                SELECT u.id,u.username,u.public_id,
                       COALESCE(u.last_seen > now()-INTERVAL '35 seconds',false),
                       COALESCE(NULLIF(u.display_name,''),u.username),u.bio,u.avatar_theme,
                       EXISTS (SELECT 1 FROM muted_contacts mc
                               WHERE mc.owner_id=$1 AND mc.peer_id=u.id) AS muted
                FROM friendships f
                JOIN users u ON u.id=CASE WHEN f.user_low=$1 THEN f.user_high ELSE f.user_low END
                WHERE (f.user_low=$1 OR f.user_high=$1)
                  AND NOT EXISTS (SELECT 1 FROM user_blocks b
                        WHERE (b.blocker_id=$1 AND b.blocked_id=u.id)
                           OR (b.blocker_id=u.id AND b.blocked_id=$1))
                ORDER BY lower(u.username)
            )SQL",{me.id});
            string users="[",contacts="[";
            users+="{\"id\":"+json(me.id)+",\"name\":"+json(me.name)+"}";
            for(int i=0;i<PQntuples(people.get());++i) {
                const string entry="{\"id\":"+json(Database::field(people.get(),i,0))+
                     ",\"name\":"+json(Database::field(people.get(),i,1))+
                     ",\"publicId\":"+json(Database::field(people.get(),i,2))+
                     ",\"displayName\":"+json(Database::field(people.get(),i,4))+
                     ",\"bio\":"+json(Database::field(people.get(),i,5))+
                     ",\"avatarTheme\":"+json(Database::field(people.get(),i,6))+
                     ",\"muted\":"+(Database::field(people.get(),i,7)=="t"?"true":"false")+"}";
                if(contacts.size()>1)contacts+=",";
                contacts+=entry;
                if(Database::field(people.get(),i,3)=="t")users+=","+entry;
            }
            auto requests=db.query(R"SQL(
                SELECT fr.id,fr.sender_id,fr.receiver_id,
                       other.username,other.public_id,fr.status
                FROM friend_requests fr
                JOIN users other ON other.id=CASE WHEN fr.sender_id=$1 THEN fr.receiver_id ELSE fr.sender_id END
                WHERE (fr.sender_id=$1 OR fr.receiver_id=$1) AND fr.status='pending'
                ORDER BY fr.created_at DESC LIMIT 100
            )SQL",{me.id});
            string incoming="[",outgoing="[";
            for(int i=0;i<PQntuples(requests.get());++i) {
                const string entry="{\"requestId\":"+Database::field(requests.get(),i,0)+
                     ",\"name\":"+json(Database::field(requests.get(),i,3))+
                     ",\"publicId\":"+json(Database::field(requests.get(),i,4))+"}";
                string& list=(Database::field(requests.get(),i,2)==me.id?incoming:outgoing);
                if(list.size()>1)list+=",";
                list+=entry;
            }
            auto blockedRows=db.query(R"SQL(
                SELECT u.username,u.public_id
                FROM user_blocks b JOIN users u ON u.id=b.blocked_id
                WHERE b.blocker_id=$1 ORDER BY b.created_at DESC LIMIT 200
            )SQL",{me.id});
            string blocked="[";
            for(int i=0;i<PQntuples(blockedRows.get());++i) {
                if(blocked.size()>1)blocked+=",";
                blocked+="{\"name\":"+json(Database::field(blockedRows.get(),i,0))+
                        ",\"publicId\":"+json(Database::field(blockedRows.get(),i,1))+"}";
            }
            // Calculate unread messages from durable server-side read markers.
            // Never return unread metadata for strangers.
            auto unreadRows=db.query(R"SQL(
              SELECT m.author_id, count(*)::TEXT
              FROM messages m
              JOIN friendships f
                ON f.user_low=LEAST(m.author_id,m.recipient_id)
               AND f.user_high=GREATEST(m.author_id,m.recipient_id)
              LEFT JOIN chat_read_markers r
                ON r.reader_id=$1 AND r.sender_id=m.author_id
              WHERE m.recipient_id=$1
                AND NOT EXISTS (SELECT 1 FROM user_blocks b
                    WHERE (b.blocker_id=$1 AND b.blocked_id=m.author_id)
                       OR (b.blocker_id=m.author_id AND b.blocked_id=$1))
                AND m.deleted_at IS NULL
                AND NOT EXISTS (SELECT 1 FROM message_hidden_for_user h
                                WHERE h.user_id=$1 AND h.message_id=m.id)
                AND m.id > COALESCE(r.last_read_id,0)
              GROUP BY m.author_id
            )SQL",{me.id});
            string unread="{";
            for(int i=0;i<PQntuples(unreadRows.get());++i) {
                if(unread.size()>1)unread+=",";
                unread+=json(Database::field(unreadRows.get(),i,0))+":"+
                        Database::field(unreadRows.get(),i,1);
            }
            unread+="}";
            auto rows=db.query(R"SQL(
              -- Notes belong only to their author. Historical DMs remain hidden
              -- until the two accounts have an accepted connection.
              SELECT id,author_id,username,COALESCE(recipient_id,''),body,time,delivery,edited,deleted,attachment_id,attachment_name,attachment_mime,attachment_size,reply_json,reactions_json FROM (
                SELECT m.id,m.author_id,u.username,m.recipient_id,
                       CASE WHEN m.deleted_at IS NOT NULL THEN '' ELSE m.body END AS body,
                       EXTRACT(EPOCH FROM m.created_at)::BIGINT AS time,
                       CASE WHEN m.author_id=$1 AND m.recipient_id IS NOT NULL THEN
                         CASE WHEN m.seen_at IS NOT NULL THEN 'seen'
                              WHEN m.delivered_at IS NOT NULL THEN 'delivered'
                              ELSE 'sent' END ELSE '' END AS delivery,
                       (m.edited_at IS NOT NULL) AS edited,
                       (m.deleted_at IS NOT NULL) AS deleted,
                       CASE WHEN m.deleted_at IS NULL THEN COALESCE(a.id::TEXT,'') ELSE '' END AS attachment_id,
                       COALESCE(a.filename,'') AS attachment_name,COALESCE(a.mime,'') AS attachment_mime,
                       COALESCE(a.size_bytes::TEXT,'0') AS attachment_size,
                       CASE WHEN rm.id IS NULL THEN 'null' ELSE json_build_object(
                         'id',rm.id,'name',COALESCE(NULLIF(ru.display_name,''),ru.username),
                         'text',CASE WHEN rm.deleted_at IS NOT NULL THEN '[Deleted message]'
                             WHEN ra.id IS NOT NULL THEN '[Attachment] '||ra.filename
                             ELSE LEFT(rm.body,120) END)::TEXT END AS reply_json,
                       COALESCE((SELECT json_agg(json_build_object('emoji',q.emoji,'count',q.cnt,'mine',q.mine))::TEXT
                           FROM (SELECT emoji,COUNT(*)::INT AS cnt,BOOL_OR(user_id=$1::VARCHAR(32)) AS mine
                                 FROM cc_dm_reactions WHERE message_id=m.id GROUP BY emoji ORDER BY emoji) q),'[]') AS reactions_json
                FROM messages m JOIN users u ON u.id=m.author_id
                LEFT JOIN cc_attachments a ON a.dm_message_id=m.id
                LEFT JOIN messages rm ON rm.id=m.reply_to_id AND NOT EXISTS (
                    SELECT 1 FROM message_hidden_for_user h WHERE h.user_id=$1::VARCHAR(32) AND h.message_id=rm.id)
                LEFT JOIN users ru ON ru.id=rm.author_id
                LEFT JOIN cc_attachments ra ON ra.dm_message_id=rm.id
                WHERE NOT EXISTS (SELECT 1 FROM message_hidden_for_user h
                                  WHERE h.user_id=$1 AND h.message_id=m.id)
                  AND ((m.recipient_id IS NULL AND m.author_id=$1)
                   OR (m.recipient_id IS NOT NULL AND (m.author_id=$1 OR m.recipient_id=$1)
                       AND EXISTS (SELECT 1 FROM friendships f
                                   WHERE f.user_low=LEAST(m.author_id,m.recipient_id)
                                     AND f.user_high=GREATEST(m.author_id,m.recipient_id)
                                     AND NOT EXISTS (SELECT 1 FROM user_blocks b
                                       WHERE (b.blocker_id=m.author_id AND b.blocked_id=m.recipient_id)
                                          OR (b.blocker_id=m.recipient_id AND b.blocked_id=m.author_id)))))
                ORDER BY m.id DESC LIMIT 300
              ) visible_messages ORDER BY id ASC
            )SQL",{me.id});
            string messages="[";
            for(int i=0;i<PQntuples(rows.get());++i) {
                if(messages.size()>1)messages+=",";
                messages+="{\"id\":"+Database::field(rows.get(),i,0)+",\"author\":"+json(Database::field(rows.get(),i,1))+
                    ",\"name\":"+json(Database::field(rows.get(),i,2))+",\"to\":"+json(Database::field(rows.get(),i,3))+
                    ",\"text\":"+json(Database::field(rows.get(),i,4))+",\"time\":"+Database::field(rows.get(),i,5)+
                    ",\"delivery\":"+json(Database::field(rows.get(),i,6))+
                    ",\"edited\":"+(Database::field(rows.get(),i,7)=="t"?"true":"false")+
                    ",\"deleted\":"+(Database::field(rows.get(),i,8)=="t"?"true":"false")+
                    ",\"attachment\":"+(Database::field(rows.get(),i,9).empty()?"null":
                       "{\"id\":"+Database::field(rows.get(),i,9)+
                       ",\"name\":"+json(Database::field(rows.get(),i,10))+
                       ",\"mime\":"+json(Database::field(rows.get(),i,11))+
                       ",\"size\":"+Database::field(rows.get(),i,12)+"}")+
                    ",\"reply\":"+Database::field(rows.get(),i,13)+
                    ",\"reactions\":"+Database::field(rows.get(),i,14)+"}";
            }
            // Never expose wallpapers for strangers, blocked accounts or ex-friends.
            auto wallpaperRows=db.query(R"SQL(
                SELECT CASE WHEN w.user_low=$1 THEN w.user_high ELSE w.user_low END,
                       w.wallpaper_id
                FROM cc_dm_wallpapers w
                JOIN friendships f ON f.user_low=w.user_low AND f.user_high=w.user_high
                WHERE (w.user_low=$1 OR w.user_high=$1)
                  AND NOT EXISTS (SELECT 1 FROM user_blocks b
                    WHERE (b.blocker_id=w.user_low AND b.blocked_id=w.user_high)
                       OR (b.blocker_id=w.user_high AND b.blocked_id=w.user_low))
            )SQL",{me.id});
            string dmWallpapers="{";
            for(int i=0;i<PQntuples(wallpaperRows.get());++i){
                if(dmWallpapers.size()>1)dmWallpapers+=",";
                dmWallpapers+=json(Database::field(wallpaperRows.get(),i,0))+":"+
                              json(Database::field(wallpaperRows.get(),i,1));
            }
            dmWallpapers+="}";
            auto groupRows=db.query(R"SQL(
                SELECT g.id,g.name,g.owner_id,
                       COALESCE(NULLIF(owner.display_name,''),owner.username),
                       g.description,g.photo_data,g.wallpaper_id,
                       EXISTS (SELECT 1 FROM cc_group_mutes mu WHERE mu.group_id=g.id AND mu.user_id=$1)
                FROM cc_groups g
                JOIN cc_group_members gm ON gm.group_id=g.id AND gm.user_id=$1
                JOIN users owner ON owner.id=g.owner_id
                ORDER BY lower(g.name),g.created_at DESC LIMIT 100
            )SQL",{me.id});
            // Every group member can see the other members' names within that group.
            auto memberRows=db.query(R"SQL(
                SELECT m.group_id,u.id,u.username,COALESCE(NULLIF(u.display_name,''),u.username)
                FROM cc_group_members m
                JOIN users u ON u.id=m.user_id
                WHERE EXISTS(SELECT 1 FROM cc_group_members mine
                             WHERE mine.group_id=m.group_id AND mine.user_id=$1)
                ORDER BY lower(u.username)
            )SQL",{me.id});
            string groups="[";
            for(int i=0;i<PQntuples(groupRows.get());++i) {
                const string group=Database::field(groupRows.get(),i,0);
                if(groups.size()>1)groups+=",";
                groups+="{\"id\":"+json(group)+",\"name\":"+json(Database::field(groupRows.get(),i,1))+
                        ",\"ownerId\":"+json(Database::field(groupRows.get(),i,2))+
                        ",\"ownerName\":"+json(Database::field(groupRows.get(),i,3))+
                        ",\"description\":"+json(Database::field(groupRows.get(),i,4))+
                        ",\"photoData\":"+json(Database::field(groupRows.get(),i,5))+
                        ",\"wallpaperId\":"+json(Database::field(groupRows.get(),i,6))+
                        ",\"muted\":"+(Database::field(groupRows.get(),i,7)=="t"?"true":"false")+
                        ",\"members\":[";
                bool firstMember=true;
                for(int j=0;j<PQntuples(memberRows.get());++j) {
                    if(Database::field(memberRows.get(),j,0)!=group)continue;
                    if(!firstMember)groups+=",";
                    firstMember=false;
                    groups+="{\"id\":"+json(Database::field(memberRows.get(),j,1))+
                            ",\"name\":"+json(Database::field(memberRows.get(),j,2))+
                            ",\"displayName\":"+json(Database::field(memberRows.get(),j,3))+"}";
                }
                groups+="]}";
            }
            auto groupInvites=db.query(R"SQL(
                SELECT i.id,g.id,g.name,u.username
                FROM cc_group_invites i
                JOIN cc_groups g ON g.id=i.group_id
                JOIN users u ON u.id=i.inviter_id
                WHERE i.invitee_id=$1 AND i.status='pending'
                  AND EXISTS(SELECT 1 FROM cc_group_members gm
                             WHERE gm.group_id=i.group_id AND gm.user_id=i.inviter_id)
                ORDER BY i.created_at DESC LIMIT 100
            )SQL",{me.id});
            string invitations="[";
            for(int i=0;i<PQntuples(groupInvites.get());++i) {
                if(invitations.size()>1)invitations+=",";
                invitations+="{\"inviteId\":"+Database::field(groupInvites.get(),i,0)+
                             ",\"groupId\":"+json(Database::field(groupInvites.get(),i,1))+
                             ",\"name\":"+json(Database::field(groupInvites.get(),i,2))+
                             ",\"inviter\":"+json(Database::field(groupInvites.get(),i,3))+"}";
            }
            auto groupMessages=db.query(R"SQL(
                SELECT id,group_id,author_id,username,body,time,edited,deleted,attachment_id,attachment_name,attachment_mime,attachment_size,reply_json,reactions_json FROM (
                  SELECT m.id,m.group_id,m.author_id,u.username,m.body,
                         EXTRACT(EPOCH FROM m.created_at)::BIGINT AS time,
                         (m.edited_at IS NOT NULL) AS edited,
                         (m.deleted_at IS NOT NULL) AS deleted,
                         CASE WHEN m.deleted_at IS NULL THEN COALESCE(a.id::TEXT,'') ELSE '' END AS attachment_id,
                         COALESCE(a.filename,'') AS attachment_name,COALESCE(a.mime,'') AS attachment_mime,
                         COALESCE(a.size_bytes::TEXT,'0') AS attachment_size,
                         CASE WHEN rm.id IS NULL THEN 'null' ELSE json_build_object(
                           'id',rm.id,'name',COALESCE(NULLIF(ru.display_name,''),ru.username),
                           'text',CASE WHEN rm.deleted_at IS NOT NULL THEN '[Deleted message]'
                               WHEN ra.id IS NOT NULL THEN '[Attachment] '||ra.filename
                               ELSE LEFT(rm.body,120) END)::TEXT END AS reply_json,
                         COALESCE((SELECT json_agg(json_build_object('emoji',q.emoji,'count',q.cnt,'mine',q.mine))::TEXT
                             FROM (SELECT emoji,COUNT(*)::INT AS cnt,BOOL_OR(user_id=$1::VARCHAR(32)) AS mine
                                   FROM cc_group_reactions WHERE message_id=m.id GROUP BY emoji ORDER BY emoji) q),'[]') AS reactions_json
                  FROM cc_group_messages m
                  JOIN cc_group_members gm ON gm.group_id=m.group_id AND gm.user_id=$1
                  JOIN users u ON u.id=m.author_id
                  LEFT JOIN cc_attachments a ON a.group_message_id=m.id
                  LEFT JOIN cc_group_messages rm ON rm.id=m.reply_to_id AND rm.created_at>=gm.joined_at
                    AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                                   WHERE h.message_id=rm.id AND h.user_id=$1::VARCHAR(32))
                    AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                   WHERE (b.blocker_id=$1::VARCHAR(32) AND b.blocked_id=rm.author_id)
                                      OR (b.blocker_id=rm.author_id AND b.blocked_id=$1::VARCHAR(32)))
                  LEFT JOIN users ru ON ru.id=rm.author_id
                  LEFT JOIN cc_attachments ra ON ra.group_message_id=rm.id
                  WHERE m.created_at>=gm.joined_at
                    AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                                   WHERE h.message_id=m.id AND h.user_id=$1)
                    AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                   WHERE (b.blocker_id=$1 AND b.blocked_id=m.author_id)
                                      OR (b.blocker_id=m.author_id AND b.blocked_id=$1))
                  ORDER BY m.id DESC LIMIT 500
                ) recent ORDER BY id ASC
            )SQL",{me.id});
            string groupFeed="[";
            for(int i=0;i<PQntuples(groupMessages.get());++i) {
                if(groupFeed.size()>1)groupFeed+=",";
                groupFeed+="{\"id\":"+Database::field(groupMessages.get(),i,0)+
                    ",\"groupId\":"+json(Database::field(groupMessages.get(),i,1))+
                    ",\"author\":"+json(Database::field(groupMessages.get(),i,2))+
                    ",\"name\":"+json(Database::field(groupMessages.get(),i,3))+
                    ",\"text\":"+json(Database::field(groupMessages.get(),i,4))+
                    ",\"time\":"+Database::field(groupMessages.get(),i,5)+
                    ",\"edited\":"+(Database::field(groupMessages.get(),i,6)=="t"?"true":"false")+
                    ",\"deleted\":"+(Database::field(groupMessages.get(),i,7)=="t"?"true":"false")+
                    ",\"attachment\":"+(Database::field(groupMessages.get(),i,8).empty()?"null":
                       "{\"id\":"+Database::field(groupMessages.get(),i,8)+
                       ",\"name\":"+json(Database::field(groupMessages.get(),i,9))+
                       ",\"mime\":"+json(Database::field(groupMessages.get(),i,10))+
                       ",\"size\":"+Database::field(groupMessages.get(),i,11)+"}")+
                    ",\"reply\":"+Database::field(groupMessages.get(),i,12)+
                    ",\"reactions\":"+Database::field(groupMessages.get(),i,13)+"}";
            }
            auto groupUnread=db.query(R"SQL(
                SELECT m.group_id,count(*)::TEXT
                FROM cc_group_messages m
                JOIN cc_group_members gm ON gm.group_id=m.group_id AND gm.user_id=$1
                LEFT JOIN cc_group_read_markers r
                  ON r.group_id=m.group_id AND r.reader_id=$1
                WHERE m.created_at>=gm.joined_at AND m.author_id<>$1
                  AND m.deleted_at IS NULL
                  AND NOT EXISTS(SELECT 1 FROM cc_group_message_hides h
                                 WHERE h.message_id=m.id AND h.user_id=$1)
                  AND m.id>COALESCE(r.last_read_id,0)
                  AND NOT EXISTS(SELECT 1 FROM user_blocks b
                                 WHERE (b.blocker_id=$1 AND b.blocked_id=m.author_id)
                                    OR (b.blocker_id=m.author_id AND b.blocked_id=$1))
                GROUP BY m.group_id
            )SQL",{me.id});
            string groupUnreadJson="{";
            for(int i=0;i<PQntuples(groupUnread.get());++i) {
                if(groupUnreadJson.size()>1)groupUnreadJson+=",";
                groupUnreadJson+=json(Database::field(groupUnread.get(),i,0))+":"+
                                 Database::field(groupUnread.get(),i,1);
            }
            groupUnreadJson+="}";
            std::lock_guard<std::mutex> lock(mutex);expire();string composing="[";
            for(const auto& item:typing) {
                if(item.first==me.id)continue;
                const auto& state=item.second;
                // A typing signal can only be seen inside a currently authorized
                // private conversation. Former/removed members and blocks see nothing.
                if(!state.group.empty()) {
                    if(!isGroupMember(db,state.group,me.id) ||
                       !isGroupMember(db,state.group,item.first) ||
                       store.isBlockedEitherWay(db,me.id,item.first))continue;
                } else {
                    if(state.to!=me.id || !store.areFriends(db,me.id,item.first))continue;
                }
                if(composing.size()>1)composing+=",";
                composing+="{\"author\":"+json(item.first)+",\"name\":"+json(state.name)+
                    ",\"to\":"+json(state.to)+",\"groupId\":"+json(state.group)+"}";
            }
            return "{\"publicId\":"+json(me.publicId)+",\"unread\":"+unread+",\"users\":"+users+"],\"contacts\":"+contacts+"],\"incoming\":"+incoming+
                   "],\"outgoing\":"+outgoing+"],\"blocked\":"+blocked+
                   "],\"messages\":"+messages+"],\"typing\":"+composing+"],\"dmWallpapers\":"+dmWallpapers+",\"groups\":"+groups+"],\"groupInvites\":"+invitations+"],\"groupMessages\":"+groupFeed+"],\"groupUnread\":"+groupUnreadJson+"}";
        }
        if(r.path=="/api/delivered"&&r.method=="POST") {
            const string from=data["from"],ids=data["ids"];
            if(from.empty() || from==me.id || !store.areFriends(db,me.id,from))
                throw Problem{403,"Only connected friends can acknowledge delivery."};
            if(ids.empty() || ids.size()>6500 || !std::regex_match(ids,std::regex("[1-9][0-9]{0,18}(,[1-9][0-9]{0,18}){0,299}")))
                throw Problem{400,"Invalid delivery receipt IDs."};
            // Confirm exactly those message IDs that the authenticated browser
            // received in /api/state (not an entire earlier unseen history).
            db.query(R"SQL(
              UPDATE messages SET delivered_at=now()
              WHERE author_id=$1 AND recipient_id=$2
                AND id=ANY(string_to_array($3,',')::BIGINT[])
                AND delivered_at IS NULL
            )SQL",{from,me.id,ids});
            events.publish(from); // Immediately update sender's ✓✓ delivery indicator.
            return "{\"status\":\"delivered\"}";
        }
        if(r.path=="/api/read"&&r.method=="POST") {
            const string from=data["from"],upTo=data["up_to"];
            if(from.empty()||from==me.id||!store.areFriends(db,me.id,from))
                throw Problem{403,"Only connected friends can be marked as read."};
            if(upTo.empty()||upTo.size()>19||upTo.find_first_not_of("0123456789")!=string::npos)
                throw Problem{400,"Invalid message read marker."};
            long long lastId=0;
            try {lastId=std::stoll(upTo);} catch(...) {throw Problem{400,"Invalid message read marker."};}
            if(lastId<=0)throw Problem{400,"Invalid message read marker."};
            // Clamp the marker to a message that this reader could actually see.
            // This prevents fabricated huge IDs from marking future messages as read.
            // Resolve the highest actually visible message first, then upsert it.
            // Keeping the aggregate SELECT separate from INSERT ... ON CONFLICT
            // avoids a PostgreSQL query failure on the original combined statement.
            const auto latest=db.query(R"SQL(
              SELECT COALESCE(MAX(id),0)::TEXT
              FROM messages
              WHERE author_id=$1 AND recipient_id=$2 AND id <= $3::BIGINT
            )SQL",{from,me.id,std::to_string(lastId)});
            const string verifiedId=Database::field(latest.get(),0,0);
            if(verifiedId!="0" && !verifiedId.empty()) {
                db.query("BEGIN");
                db.query(R"SQL(
                  INSERT INTO chat_read_markers(reader_id,sender_id,last_read_id,updated_at)
                  VALUES ($1::VARCHAR(32),$2::VARCHAR(32),$3::BIGINT,now())
                  ON CONFLICT (reader_id,sender_id)
                  DO UPDATE SET last_read_id=GREATEST(chat_read_markers.last_read_id,EXCLUDED.last_read_id),
                                updated_at=now()
                )SQL",{me.id,from,verifiedId});
                db.query(R"SQL(
                  UPDATE messages
                  SET delivered_at=COALESCE(delivered_at,now()),
                      seen_at=COALESCE(seen_at,now())
                  WHERE author_id=$1 AND recipient_id=$2 AND id<=$3::BIGINT
                )SQL",{from,me.id,verifiedId});
                db.query("COMMIT");
                events.publish(from); // Seen ✓✓ should update immediately.
            }
            return "{\"status\":\"read\"}";
        }
        // Edit/Delete for Everyone are author-only; Delete for Me is allowed
        // to either participant and only hides the row for the signed-in user.
        // The browser never chooses which account the hide applies to.
        if((r.path=="/api/messages/edit"||r.path=="/api/messages/delete"||
            r.path=="/api/messages/delete-me")&&r.method=="POST") {
            const string rawId=data["id"];
            if(rawId.empty() || rawId.size()>19 || rawId[0]=='0' ||
               rawId.find_first_not_of("0123456789")!=string::npos)
                throw Problem{400,"Invalid message ID."};
            long long parsedId=0;
            try { parsedId=std::stoll(rawId); } catch(...) { throw Problem{400,"Invalid message ID."}; }
            if(parsedId<=0)throw Problem{400,"Invalid message ID."};
            if(r.path=="/api/messages/delete-me") {
                // Do not leak or allow hiding a stranger's message by ID.
                const auto allowed=db.query(R"SQL(
                  SELECT id FROM messages
                  WHERE id=$2::BIGINT AND (author_id=$1 OR recipient_id=$1)
                )SQL",{me.id,std::to_string(parsedId)});
                if(PQntuples(allowed.get())==0)
                    throw Problem{404,"Message not found for this account."};
                // Idempotent: repeated requests are harmless, including after refresh.
                db.query(R"SQL(
                  INSERT INTO message_hidden_for_user(user_id,message_id)
                  VALUES($1,$2::BIGINT)
                  ON CONFLICT (user_id,message_id) DO NOTHING
                )SQL",{me.id,std::to_string(parsedId)});
                events.publish(me.id); // Only THIS account updates; friend unaffected.
                return "{\"status\":\"deleted_for_me\"}";
            }
            const bool editing=r.path=="/api/messages/edit";
            const string body=data["text"];
            if(editing && (body.empty() || body.size()>2000 ||
                body.find_first_not_of(" \t\n\r")==string::npos))
                throw Problem{400,"Write a message up to 2000 bytes."};
            if(editing) {
                auto file=db.query("SELECT 1 FROM cc_attachments WHERE dm_message_id=$1::BIGINT",{std::to_string(parsedId)});
                if(PQntuples(file.get()))throw Problem{409,"Attachments cannot be edited; delete and resend instead."};
            }
            Database::Result changed=editing
                ? db.query(R"SQL(
                    UPDATE messages SET body=$3, edited_at=now()
                    WHERE id=$2::BIGINT AND author_id=$1 AND deleted_at IS NULL
                    RETURNING COALESCE(recipient_id,'')
                  )SQL",{me.id,std::to_string(parsedId),body})
                : db.query(R"SQL(
                    UPDATE messages SET body='', deleted_at=now()
                    WHERE id=$2::BIGINT AND author_id=$1 AND deleted_at IS NULL
                    RETURNING COALESCE(recipient_id,'')
                  )SQL",{me.id,std::to_string(parsedId)});
            if(PQntuples(changed.get())==0)
                throw Problem{404,"Message not found or not editable by your account."};
            if(!editing)db.query("DELETE FROM cc_attachments WHERE dm_message_id=$1::BIGINT",{std::to_string(parsedId)});
            const auto recipient=Database::field(changed.get(),0,0);
            events.publish(recipient);
            events.publish(me.id);
            return "{\"status\":"+json(editing?"edited":"deleted")+"}";
        }
        if(r.path=="/api/messages/react" && r.method=="POST") {
            const string id=data["id"],emoji=data["emoji"];
            if(!validSqlNumber(id)||(!emoji.empty()&&!validReactionEmoji(emoji)))
                throw Problem{400,"Invalid reaction."};
            const auto row=db.query(R"SQL(
                SELECT m.author_id,m.recipient_id FROM messages m
                WHERE m.id=$2::BIGINT AND m.deleted_at IS NULL AND m.recipient_id IS NOT NULL
                  AND (m.author_id=$1::VARCHAR(32) OR m.recipient_id=$1::VARCHAR(32))
                  AND NOT EXISTS(SELECT 1 FROM message_hidden_for_user h
                                 WHERE h.user_id=$1::VARCHAR(32) AND h.message_id=m.id)
            )SQL",{me.id,id});
            if(!PQntuples(row.get()))throw Problem{404,"Private message unavailable."};
            const string author=Database::field(row.get(),0,0),to=Database::field(row.get(),0,1);
            const string peer=author==me.id?to:author;
            if(!store.areFriends(db,me.id,peer))throw Problem{403,"Connect as friends before reacting."};
            if(emoji.empty())db.query("DELETE FROM cc_dm_reactions WHERE message_id=$1::BIGINT AND user_id=$2::VARCHAR(32)",{id,me.id});
            else db.query(R"SQL(
                INSERT INTO cc_dm_reactions(message_id,user_id,emoji)
                VALUES ($1::BIGINT,$2::VARCHAR(32),$3::TEXT)
                ON CONFLICT(message_id,user_id) DO UPDATE SET emoji=EXCLUDED.emoji
            )SQL",{id,me.id,emoji});
            events.publish(peer);events.publish(me.id);
            return "{\"status\":\"updated\"}";
        }
        if(r.path=="/api/send"&&r.method=="POST") {
            const auto message=data["text"],to=data["to"],reply=data["reply_to"],clientRef=data["client_ref"];
            if(!clientRef.empty() && !validClientMessageRef(clientRef))
                throw Problem{400,"Invalid client message reference."};
            if(message.empty()||message.size()>2000||message.find_first_not_of(" \t\n\r")==string::npos)
                throw Problem{400,"Write a message up to 2000 bytes."};
            if(!to.empty()) {
                if(to==me.id)throw Problem{400,"Choose another person."};
                if(!store.areFriends(db,me.id,to))throw Problem{403,"Connect as friends before messaging."};
            }
            if(!reply.empty()) {
                if(!validSqlNumber(reply)||to.empty())throw Problem{400,"Invalid reply target."};
                auto original=db.query(R"SQL(
                    SELECT 1 FROM messages m WHERE m.id=$2::BIGINT AND m.deleted_at IS NULL
                      AND ((m.author_id=$1::VARCHAR(32) AND m.recipient_id=$3::VARCHAR(32))
                        OR (m.author_id=$3::VARCHAR(32) AND m.recipient_id=$1::VARCHAR(32)))
                      AND NOT EXISTS(SELECT 1 FROM message_hidden_for_user h
                                     WHERE h.user_id=$1::VARCHAR(32) AND h.message_id=m.id)
                )SQL",{me.id,reply,to});
                if(!PQntuples(original.get()))throw Problem{403,"Cannot reply to a message outside this conversation."};
            }
            string messageId;
            if(clientRef.empty()) {
                auto inserted=db.query(R"SQL(
                    INSERT INTO messages(author_id,recipient_id,body,reply_to_id)
                    VALUES($1::VARCHAR(32),NULLIF($2::TEXT,'')::VARCHAR(32),$3::TEXT,NULLIF($4::TEXT,'')::BIGINT)
                    RETURNING id
                )SQL",{me.id,to,message,reply});
                messageId=Database::field(inserted.get(),0,0);
            } else {
                const string fingerprint=digest("dm\n"+to+"\n"+message+"\n"+reply);
                auto inserted=db.query(R"SQL(
                    INSERT INTO messages(author_id,recipient_id,body,reply_to_id,client_ref,client_payload_hash)
                    VALUES($1::VARCHAR(32),NULLIF($2::TEXT,'')::VARCHAR(32),$3::TEXT,
                           NULLIF($4::TEXT,'')::BIGINT,$5::VARCHAR(32),$6::VARCHAR(64))
                    ON CONFLICT (author_id,client_ref) WHERE client_ref IS NOT NULL DO NOTHING
                    RETURNING id
                )SQL",{me.id,to,message,reply,clientRef,fingerprint});
                if(!PQntuples(inserted.get())) {
                    auto previous=db.query(R"SQL(
                        SELECT id::TEXT,client_payload_hash FROM messages
                        WHERE author_id=$1::VARCHAR(32) AND client_ref=$2::VARCHAR(32)
                    )SQL",{me.id,clientRef});
                    if(!PQntuples(previous.get()))throw Problem{503,"Please retry the message."};
                    if(Database::field(previous.get(),0,1)!=fingerprint)
                        throw Problem{409,"Message reference already used for different content."};
                    return "{\"id\":"+Database::field(previous.get(),0,0)+",\"deduplicated\":true}";
                }
                messageId=Database::field(inserted.get(),0,0);
            }
            { std::lock_guard<std::mutex> lock(mutex);typing.erase(me.id); }
            events.publish(to); // Wake the intended receiver, never strangers.
            return "{\"id\":"+messageId+",\"deduplicated\":false}";
        }
        if(r.path=="/api/typing"&&r.method=="POST") {
            // The server determines the author from the bearer token. Never accept
            // a client-supplied author/name or broadcast to arbitrary account IDs.
            typingLimiter.enforce("typing:"+me.id,90,std::chrono::minutes(1));
            const string to=data["to"],group=data["group_id"];
            const bool active=data["typing"]=="1";
            if(active) {
                if((to.empty()==group.empty()) || to==me.id)
                    throw Problem{400,"Choose one friend or group for typing."};
                if(!group.empty()) {
                    if(!validGroupId(group))throw Problem{400,"Invalid group ID."};
                    if(!isGroupMember(db,group,me.id))throw Problem{403,"Not a group member."};
                } else if(!store.areFriends(db,me.id,to))
                    throw Problem{403,"Connect as friends before typing."};
            }
            TypingState previous{"","","",0};
            bool hadPrevious=false;
            {
                std::lock_guard<std::mutex> lock(mutex);expire();
                auto it=typing.find(me.id);
                if(it!=typing.end()){previous=it->second;hadPrevious=true;}
                if(active)typing[me.id]=TypingState{me.name,to,group,now()};
                else typing.erase(me.id);
            }
            // Wake only authorized conversation participants. Also wake the old
            // recipient when switching chats, so their indicator clears promptly.
            if(hadPrevious && (previous.to!=to || previous.group!=group || !active)) {
                if(!previous.group.empty())publishGroup(db,previous.group);
                else events.publish(previous.to);
            }
            if(active) {
                if(!group.empty())publishGroup(db,group);
                else events.publish(to);
            }
            return "{}";
        }
        throw Problem{404,"Request not found"};
    }
};
void respond(Socket& s,int code,const string& type,const string& body) {
    const string retry = code==429 ? "Retry-After: 900\r\n" : "";
    s.write("HTTP/1.1 "+std::to_string(code)+(code==200?" OK":" Error")+"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; style-src 'self'; script-src 'self'; base-uri 'none'; frame-ancestors 'none'\r\n"+retry+"Content-Type: "+type+"\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body);
}
string safeDownloadName(const string& name) {
    string safe;
    for(unsigned char c:name){
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'||c==' ')
            safe+=char(c);
        else safe+='_';
    }
    if(safe.empty())safe="attachment";
    return safe;
}
void respondAttachment(Socket& socket,const ChatRoom::AttachmentDownload& file,bool preview){
    std::ifstream stream(file.path,std::ios::binary);
    if(!stream)throw Problem{503,"Attachment cannot be opened."};
    const string filename=safeDownloadName(file.name);
    const string contentType=preview?file.mime:"application/octet-stream";
    const string disposition=preview?"inline":"attachment; filename=\""+filename+"\"";
    if(!socket.write("HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store, private\r\n"
       "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
       "Content-Security-Policy: default-src 'none'\r\nContent-Type: "+contentType+"\r\n"
       "Content-Disposition: "+disposition+"\r\nContent-Length: "+std::to_string(file.size)+"\r\n\r\n"))return;
    std::array<char,65536> buffer{};
    while(stream){
        stream.read(buffer.data(),buffer.size());
        const auto n=stream.gcount();
        if(n>0&&!socket.write(string(buffer.data(),size_t(n))))break;
    }
}
struct Asset {string type,body;};
class WebServer {
    ChatRoom room;
    std::map<string,Asset> assets;
    std::atomic<unsigned> active{0};
    EntryRateLimiter entryLimiter;
    void handle(Socket socket) {
        try {
            socket.timeout();
            const string peer=socket.peerIp();
            const auto r=readRequest(socket);
            // Don't rate-limit health/static pages or authorized messaging traffic.
            // Only sign-up and login are public, credential-bearing entry points.
            if(r.method=="POST" && r.path=="/api/signup") {
                entryLimiter.enforce("signup:"+peer,12,std::chrono::minutes(15));
            } else if(r.method=="POST" && r.path=="/api/login") {
                entryLimiter.enforce("login-ip:"+peer,80,std::chrono::minutes(5));
                const auto loginFields=fields(r.body);
                auto found=loginFields.find("identity");
                if(found!=loginFields.end() && !found->second.empty() && found->second.size()<=254) {
                    string identity=found->second;
                    std::transform(identity.begin(),identity.end(),identity.begin(),[](unsigned char ch){return char(std::tolower(ch));});
                    // Hash account name in the in-memory key, not plaintext email.
                    entryLimiter.enforce("login-id:"+digest(identity),15,std::chrono::minutes(15));
                }
            }
            auto asset=assets.find(r.path);
            if(r.method=="GET"&&r.path=="/health")respond(socket,200,"application/json","{\"status\":\"ok\",\"app\":\"ChatConnect\"}");
            else if(r.method=="GET"&&asset!=assets.end())respond(socket,200,asset->second.type,asset->second.body);
            else if(r.method=="POST" && (r.path=="/api/attachments/download" ||
                     r.path=="/api/attachments/preview" ||
                     r.path=="/api/attachments/audio")) {
                auto copy=r;const auto info=room.downloadInfo(copy);
                respondAttachment(socket,info,r.path!="/api/attachments/download");
            }
            else { auto copy=r;respond(socket,200,"application/json",room.api(copy)); }
        }catch(const Problem& p){respond(socket,p.status,"application/json","{\"error\":"+json(p.message)+"}");}
         catch(const std::exception& e){std::cerr<<"Server error: "<<e.what()<<'\n';respond(socket,500,"application/json","{\"error\":\"Server error\"}");}
        --active;
    }
public:
    WebServer() {
        for(auto file:{"index.html","style.css","app.js","cc-calls.js","cc-emoji-data.js","cc-voice-emoji.js","manifest.webmanifest","sw.js","pwa-icon-192.png","pwa-icon-512.png","pwa-icon-180.png","friends-scene.webp","orange-blossoms.webp",
                       "theme-doodle-blue.webp", "theme-kawaii-sunshine.webp", "theme-mint-blossom.webp", "theme-cat-doodle.webp", "theme-neon-love.webp", "theme-candy-clouds.webp", "theme-tropical-escape.webp", "theme-kitty-companions.webp", "theme-floating-hearts.webp", "theme-fluffy-bears.webp", "theme-pink-shoreline.webp",
                       "theme-doodle-blue-desktop.webp", "theme-kawaii-sunshine-desktop.webp", "theme-mint-blossom-desktop.webp", "theme-cat-doodle-desktop.webp", "theme-neon-love-desktop.webp", "theme-candy-clouds-desktop.webp", "theme-tropical-escape-desktop.webp", "theme-kitty-companions-desktop.webp", "theme-floating-hearts-desktop.webp", "theme-fluffy-bears-desktop.webp", "theme-pink-shoreline-desktop.webp", "theme-doodle-blue-mobile.webp", "theme-kawaii-sunshine-mobile.webp", "theme-mint-blossom-mobile.webp", "theme-cat-doodle-mobile.webp", "theme-neon-love-mobile.webp", "theme-candy-clouds-mobile.webp", "theme-tropical-escape-mobile.webp", "theme-kitty-companions-mobile.webp", "theme-floating-hearts-mobile.webp", "theme-fluffy-bears-mobile.webp", "theme-pink-shoreline-mobile.webp"}) {
            const string name=file;
            string body;
            if (!cc_embed::read(name, body)) {
                std::ifstream in(string("web/")+file,std::ios::binary);
                if(!in)throw NetworkError("Run from project folder containing web/.");
                std::ostringstream stream;stream<<in.rdbuf();body=stream.str();
            }
            const string assetMime = (name=="app.js" || name=="cc-calls.js" || name=="sw.js" || name=="cc-emoji-data.js" || name=="cc-voice-emoji.js")
              ? "text/javascript; charset=utf-8" : name=="style.css" ? "text/css; charset=utf-8"
              : name=="manifest.webmanifest" ? "application/manifest+json"
              : (name.size()>=4 && name.substr(name.size()-4)==".png") ? "image/png"
              : (name.size()>5 && name.substr(name.size()-5)==".webp") ? "image/webp"
              : "text/html; charset=utf-8";
            assets[name=="index.html"?"/":"/"+name]={assetMime,std::move(body)};
        }
    }
    void run(int port) {
        Socket listener;listener.listenAt(port);
        std::cout<<"ChatConnect accounts + PostgreSQL ready on http://localhost:"<<port<<"\n"<<std::flush;
        while(true) {
            auto client=listener.acceptOne();if(active.load()>=64)continue;++active;
            try{std::thread(&WebServer::handle,this,std::move(client)).detach();}catch(...){--active;}
        }
    }
};
#ifndef CHATCONNECT_NO_MAIN
int main(int argc,char** argv) {
    try {
        NetworkRuntime network;int port=argc==2?std::stoi(argv[1]):8085;
        if(port<1024||port>65535)throw NetworkError("Invalid port.");
        WebServer server;server.run(port);
    }catch(const Problem& p){std::cerr<<p.message<<'\n';return 1;}
     catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
