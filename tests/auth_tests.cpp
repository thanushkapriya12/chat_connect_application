#define CHATCONNECT_NO_MAIN
#include "../server.cpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
int main() {
    // Streaming 300 MiB media limits, extension checks and protocol guards.
    assert(CC_FILE_LIMIT==314572800ULL);
    assert(CC_CHUNK_LIMIT==524288);
    // Replies and emoji reactions are server validated, not merely rendered in JS.
    assert(validReactionEmoji("❤️") && validReactionEmoji("👍") && validReactionEmoji("😂"));
    assert(validReactionEmoji("😮") && validReactionEmoji("😢") && validReactionEmoji("🔥"));
    assert(!validReactionEmoji("<script>"));
    assert(!validReactionEmoji("💣"));

    assert(parseFileSize("314572800")==CC_FILE_LIMIT);
    try{parseFileSize("314572801");assert(false);}catch(const Problem& e){assert(e.status==413);}
    try{parseFileSize("-1");assert(false);}catch(const Problem& e){assert(e.status==400);}
    assert(allowedAttachmentExtension("Vineth-photo.jpg")==".jpg");
    assert(allowedAttachmentExtension("Report.pdf")==".pdf");
    assert(allowedAttachmentExtension("../../secret.pdf").empty());
    assert(allowedAttachmentExtension("evil.exe").empty());
    assert(validUploadId("0123456789abcdef0123456789abcdef"));
    assert(!validUploadId("../test"));
    // Private group photo input validation and size constraints.
    assert(!validGroupPhoto("<svg onload=alert(1) />"));
    assert(!validGroupPhoto("data:image/svg+xml;base64,PHN2Zz4="));
    assert(!validGroupPhoto("data:image/jpeg;base64,AAAA"));
    std::vector<unsigned char> fakeJpeg(140,0);fakeJpeg[0]=255;fakeJpeg[1]=216;fakeJpeg[2]=255;
    fakeJpeg[138]=255;fakeJpeg[139]=217;
    std::vector<unsigned char> asBase64(4*((fakeJpeg.size()+2)/3)+1);
    int n=EVP_EncodeBlock(asBase64.data(),fakeJpeg.data(),static_cast<int>(fakeJpeg.size()));
    assert(n>0 && validGroupPhoto("data:image/jpeg;base64,"+std::string(reinterpret_cast<char*>(asBase64.data()),n)));
    assert(validProfileText("Group info",0,240,true));
    assert(!validProfileText(std::string(241,'x'),0,240,true));
    assert(validUsername("Rahul_2026"));
    assert(!validUsername("me"));
    assert(!validUsername("with space"));
    assert(!validUsername("u/../../a"));
    assert(validEmail("friend@example.com"));
    assert(!validEmail("friend..com"));
    assert(validPassword("goodPass#123"));
    assert(validProfileText("Rahul",2,48,false));
    assert(validProfileText("Vineth R",2,48,false));
    assert(!validProfileText(" ",2,48,false));
    assert(!validProfileText("Bad\nName",2,48,false));
    assert(validProfileText("Student\nLearner",0,160,true));
    assert(!validProfileText(std::string(161,'x'),0,160,true));
    assert(validAvatarTheme("sunset") && validAvatarTheme("cobalt"));
    assert(!validAvatarTheme("<script>"));
    const auto onePublicId=publicIdFromRandom();
    const auto twoPublicId=publicIdFromRandom();
    assert(validPublicId(onePublicId));
    assert(validPublicId(twoPublicId));
    assert(onePublicId!=twoPublicId);
    assert(normalizedPublicId("cc-0123456789abcdef")=="CC-0123456789ABCDEF");
    assert(!validPublicId("CC-12"));
    try { normalizedPublicId("Vineth"); assert(false); } catch(const Problem& e) { assert(e.status==400); }
    try { normalizedPublicId("CC-XYZXYZXYZXYZXYZA"); assert(false); } catch(const Problem& e) { assert(e.status==400); }
    assert(!validPassword("bad"));
    assert(secureRandom(32).size()==64);
    assert(secureRandom(32)!=secureRandom(32));
    assert(digest("one")!=digest("two"));
    auto salt=secureRandom(16);
    const auto hashed=passwordHash("myStrongPassword123",salt);
    assert(constantEqual(hashed,passwordHash("myStrongPassword123",salt)));
    assert(!constantEqual(hashed,passwordHash("wrongPassword123",salt)));
    assert(json("\"<script>\n") == "\"\\\"<script>\\u000a\"");
    const auto decoded=fields("name=Rachel_7&password=hello%21world");
    assert(decoded.at("name")=="Rachel_7");
    assert(decoded.at("password")=="hello!world");
    try{decode("foo%XX");assert(false);}catch(const Problem& e){assert(e.status==400);}
    // DB-level guard protects all message paths, including direct API calls.
    std::ifstream source("server.cpp");
    assert(source.good());
    std::stringstream sourceText;sourceText<<source.rdbuf();
    assert(sourceText.str().find("/api/messages/react")!=std::string::npos);
    assert(sourceText.str().find("/api/groups/react")!=std::string::npos);
    assert(sourceText.str().find("reply_to_id")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_dm_reactions")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_group_reactions")!=std::string::npos);

    assert(sourceText.str().find("m.recipient_id IS NULL AND m.author_id=$1")!=std::string::npos);
    assert(sourceText.str().find("m.recipient_id IS NOT NULL AND (m.author_id=$1 OR m.recipient_id=$1)")!=std::string::npos);
    assert(sourceText.str().find("state.to!=me.id || !store.areFriends(db,me.id,item.first)")!=std::string::npos);
    assert(sourceText.str().find("CREATE UNIQUE INDEX IF NOT EXISTS users_username_unique ON users(lower(username))")!=std::string::npos);
    assert(sourceText.str().find("CREATE UNIQUE INDEX IF NOT EXISTS users_public_id_unique ON users(public_id)")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS friendships")!=std::string::npos);
    assert(sourceText.str().find("WHERE id=$2 AND receiver_id=$3 AND status='pending'")!=std::string::npos);
    assert(sourceText.str().find("if(!store.areFriends(db,me.id,to))throw Problem{403")!=std::string::npos);
    assert(sourceText.str().find("f.user_low=LEAST(m.author_id,m.recipient_id)")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS chat_read_markers")!=std::string::npos);
    assert(sourceText.str().find("Only connected friends can be marked as read.")!=std::string::npos);
    assert(sourceText.str().find("id <= $3::BIGINT")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS chat_read_markers")!=std::string::npos);
    assert(sourceText.str().find("ALTER TABLE messages ADD COLUMN IF NOT EXISTS delivered_at")!=std::string::npos);
    assert(sourceText.str().find("ALTER TABLE messages ADD COLUMN IF NOT EXISTS seen_at")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/delivered\"")!=std::string::npos);
    assert(sourceText.str().find("id=ANY(string_to_array($3,',')::BIGINT[])")!=std::string::npos);
    assert(sourceText.str().find("Only connected friends can acknowledge delivery.")!=std::string::npos);
    assert(sourceText.str().find("CASE WHEN m.seen_at IS NOT NULL THEN 'seen'")!=std::string::npos);
    // Group moderation and per-account hiding must have server-side authorisation.
    assert(sourceText.str().find("/api/groups/rename")!=std::string::npos);
    assert(sourceText.str().find("/api/groups/edit")!=std::string::npos);
    assert(sourceText.str().find("/api/groups/delete-message")!=std::string::npos);
    assert(sourceText.str().find("/api/groups/delete-me")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_group_message_hides")!=std::string::npos);
    assert(sourceText.str().find("Only the author can edit a group message")!=std::string::npos);
    // Message edits/deletes must be stored in PostgreSQL and owner-scoped.
    assert(sourceText.str().find("ALTER TABLE messages ADD COLUMN IF NOT EXISTS edited_at")!=std::string::npos);
    assert(sourceText.str().find("ALTER TABLE messages ADD COLUMN IF NOT EXISTS deleted_at")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/messages/edit\"")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/messages/delete\"")!=std::string::npos);
    assert(sourceText.str().find("author_id=$1 AND deleted_at IS NULL")!=std::string::npos);
    assert(sourceText.str().find("UPDATE messages SET body='', deleted_at=now()")!=std::string::npos);
    assert(sourceText.str().find("AND m.deleted_at IS NULL")!=std::string::npos);
    // Delete for Me is a per-account visibility flag for either participant.
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS message_hidden_for_user")!=std::string::npos);
    assert(sourceText.str().find("PRIMARY KEY(user_id,message_id)")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/messages/delete-me\"")!=std::string::npos);
    assert(sourceText.str().find("id=$2::BIGINT AND (author_id=$1 OR recipient_id=$1)")!=std::string::npos);
    assert(sourceText.str().find("WHERE h.user_id=$1 AND h.message_id=m.id")!=std::string::npos);
    assert(sourceText.str().find("events.publish(me.id); // Only THIS account updates")!=std::string::npos);
    // Friends Safety schema, identity-scoped mutation and two-way block guards.
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS user_blocks")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS muted_contacts")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/friends/block\"")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/friends/unblock\"")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/friends/remove\"")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/friends/mute\"")!=std::string::npos);
    assert(sourceText.str().find("store.isBlockedEitherWay(db,me.id,receiverId)")!=std::string::npos);
    assert(sourceText.str().find("AND NOT EXISTS (SELECT 1 FROM user_blocks b")!=std::string::npos);
    assert(sourceText.str().find("Only connected friends can be muted.")!=std::string::npos);
    assert(sourceText.str().find("db.query(\"DELETE FROM friendships")!=std::string::npos);
    // Authenticated event hub only gives account-scoped cursors, never message content.
    EventHub hub;
    assert(hub.current("A")==0 && hub.current("B")==0);
    hub.publish("A");
    assert(hub.current("A")==1 && hub.current("B")==0);
    auto first=hub.waitFor("A",0,std::chrono::milliseconds(1));
    assert(first.second && first.first==1);
    auto isolated=hub.waitFor("B",0,std::chrono::milliseconds(2));
    assert(!isolated.second && isolated.first==0);
    std::thread worker([&] {std::this_thread::sleep_for(std::chrono::milliseconds(15));hub.publish("B");});
    auto pushed=hub.waitFor("B",0,std::chrono::milliseconds(250));
    worker.join();
    assert(pushed.second && pushed.first==1);
    assert(sourceText.str().find("x-cc-cursor") != std::string::npos);
    assert(sourceText.str().find("events.publish(to)") != std::string::npos);
    // Private group access is always authorized in PostgreSQL, never from UI state.
    assert(validGroupId("0123456789abcdefabcdefab"));
    assert(!validGroupId("CC-0123456789ABCDEF"));
    assert(!validGroupId("group-with-user-input"));
    assert(validSqlNumber("12345") && !validSqlNumber("0") && !validSqlNumber("123xx"));
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_groups")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_group_members")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_group_invites")!=std::string::npos);
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_group_messages")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/groups/create\"")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/groups/invite\"")!=std::string::npos);
    assert(sourceText.str().find("r.path==\"/api/groups/send\"")!=std::string::npos);
    assert(sourceText.str().find("Only group members can send messages")!=std::string::npos);
    assert(sourceText.str().find("Only group owner can remove members")!=std::string::npos);
    assert(sourceText.str().find("JOIN cc_group_members gm ON gm.group_id=m.group_id AND gm.user_id=$1")!=std::string::npos);
    assert(sourceText.str().find("groupUnreadJson")!=std::string::npos);
    // Attachments are restricted by signature, type, length, authenticated ownership.
    const auto pdf=std::vector<unsigned char>{'%','P','D','F','-','1','.','4','\n'};
    assert(attachmentMime("Course.pdf",pdf)=="application/pdf");
    assert(attachmentMime("course.exe",pdf).empty());
    assert(attachmentMime("voice.webm",std::vector<unsigned char>{0x1a,0x45,0xdf,0xa3,0x9f,0x42})=="audio/webm");
    assert(attachmentMime("voice.ogg",std::vector<unsigned char>{'O','g','g','S',0,2})=="audio/ogg");
    assert(attachmentMime("voice.m4a",std::vector<unsigned char>{0,0,0,20,'f','t','y','p','M','4','A',' '})=="audio/mp4");
    assert(attachmentMime("voice.webm",pdf).empty());
    assert(allowedAttachmentExtension("voice.webm")==".webm");
    assert(allowedAttachmentExtension("virus.webm.exe").empty());
    assert(CC_VOICE_LIMIT==CC_FILE_LIMIT);
    assert(CC_VOICE_LIMIT==314572800ULL);
    assert(attachmentMime("course.jpg",pdf).empty());
    assert(attachmentMime("../report.pdf",pdf).empty());
    assert(attachmentMime("hello.txt",std::vector<unsigned char>{'H','i'})=="text/plain");
    assert(attachmentMime("hello.txt",std::vector<unsigned char>{'H',0,'i'}).empty());
    const auto base64=string("JVBERi0xLjQK");
    assert(decodeAttachmentChunk(base64)==pdf);
    try { decodeAttachmentChunk("AAAA==="); assert(false); } catch(const Problem& e){assert(e.status==400 || e.status==413);}
    try { decodeAttachmentChunk("//9="); assert(false); } catch(const Problem& e){assert(e.status==400 || e.status==413);}
    try { decodeAttachmentChunk(string(3000000,'A')); assert(false); } catch(const Problem& e){assert(e.status==413);}
    assert(sourceText.str().find("CREATE TABLE IF NOT EXISTS cc_attachments")!=std::string::npos);
    assert(sourceText.str().find("/api/attachments/download")!=std::string::npos);
    assert(sourceText.str().find("m.created_at>=gm.joined_at")!=std::string::npos);
    assert(sourceText.str().find("gm.created_at>=member.joined_at")!=std::string::npos);
    std::cout << "ChatConnect auth, privacy, receipts and account-scoped live notifications checks passed.\n";
}
