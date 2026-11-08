const el = id => document.getElementById(id);
let session;
try { session = JSON.parse(sessionStorage.getItem('cc.session') || 'null'); } catch { session = null; }
let target = '', groupTarget = '', peerNames = new Map(), snapshot = {users:[],contacts:[],messages:[],typing:[],incoming:[],outgoing:[],blocked:[],unread:{},groups:[],groupInvites:[],groupMessages:[],groupUnread:{}};
let timer, busy = false, lastFeed = '', lastTypingRequest = 0, typingTimer;
let editingMessageId = null; // Keep the inline editor intact during background refreshes.
let ccReplyTo = null; // Reply target belongs to the currently selected DM/group.
let ccSearchPinned = null, ccSearchFocusId = ''; // Older server-side search hit shown inside current conversation.

const ccEmojis=['👍','❤️','😂','😮','😢','🔥'];
el('ccReplyCancel').onclick=()=>ccClearReply();
function ccClearReply(){ccReplyTo=null;ccRenderReplyBar();}
function ccRenderReplyBar(){
  const bar=el('ccReplyBar'); if(!bar)return;
  const active=ccReplyTo && ccReplyTo.key===currentDraftKey();
  bar.hidden=!active;
  if(!active)return;
  el('ccReplyName').textContent='↩ Reply to '+ccReplyTo.name;
  el('ccReplySnippet').textContent=ccReplyTo.text||'[Attachment]';
}
function ccBeginReply(row){
  if(row.deleted)return;
  ccReplyTo={key:currentDraftKey(),id:String(row.id),name:row.author===session.id?'You':row.name,
    text:row.attachment?'📎 '+row.attachment.name:row.text.slice(0,150)};
  ccRenderReplyBar();el('draft').focus();
}
async function ccReact(row,emoji,button){
  if(!session||row.deleted)return;
  button.disabled=true;el('sendError').textContent='';
  const group=groupTarget;
  try{
    const own=(row.reactions||[]).find(r=>r.emoji===emoji && r.mine);
    const picked=own?'':emoji;
    if(group) await api('groups/react',{group_id:group,id:String(row.id),emoji:picked});
    else await api('messages/react',{id:String(row.id),emoji:picked});
    lastFeed='';await refresh();
  }catch(error){el('sendError').textContent=error.message;button.disabled=false;}
}
function ccAddReactions(card,row){
  if(!target&&!groupTarget || row.deleted)return;
  const tray=document.createElement('div');tray.className='cc-reactions';
  for(const r of row.reactions||[]){
    const chip=document.createElement('button');chip.type='button';chip.className='cc-reaction-chip';
    if(r.mine)chip.classList.add('mine');
    chip.textContent=r.emoji+' '+r.count;chip.title=r.mine?'Remove your reaction':'React with '+r.emoji;
    chip.onclick=()=>ccReact(row,r.emoji,chip);tray.append(chip);
  }
  const picker=document.createElement('details');picker.className='cc-reaction-picker';
  const summary=document.createElement('summary');summary.textContent='😊+';
  summary.title='Add emoji reaction';summary.setAttribute('aria-label','Add emoji reaction');picker.append(summary);
  const options=document.createElement('div');options.className='cc-reaction-options';
  for(const emoji of ccEmojis){
    const btn=document.createElement('button');btn.type='button';btn.textContent=emoji;
    btn.title='React '+emoji;btn.onclick=()=>{picker.open=false;ccReact(row,emoji,btn);};options.append(btn);
  }
  picker.append(options);tray.append(picker);card.append(tray);
}

const drafts = new Map();
const readAcknowledged = new Map(), pendingRead = new Set(), readRetryAt = new Map();
const deliveredAcknowledged = new Map(), pendingDelivered = new Set();
let inboxInitialized = false, lastMessageByFriend = new Map(), noticeTimeout;
let knownRequests = new Set(), knownGroupInvites = new Set(), lastGroupMessages = new Map();
const readGroups = new Map(), pendingGroupRead = new Set();
let alertsEnabled = false, liveCursor = null, liveAbort = null, liveGeneration = 0;
try { alertsEnabled = localStorage.getItem('cc.desktopAlerts') === 'yes'; } catch {}
const supportsSystemAlerts = typeof Notification !== 'undefined' &&
  (window.isSecureContext || location.hostname === 'localhost' || location.hostname === '127.0.0.1');
if (alertsEnabled && (!supportsSystemAlerts || Notification.permission !== 'granted')) alertsEnabled = false;
function updateAlertsButton() {
  const button = el('alertsToggle');
  button.textContent = alertsEnabled ? '🔔 Desktop on' : '🔕 Enable desktop';
  button.setAttribute('aria-pressed', String(alertsEnabled));
  button.title = alertsEnabled ? 'Desktop notifications are on for unmuted chats. Click to turn them off.' :
    'Enable desktop notifications for unmuted chats while this website is open';
}
updateAlertsButton();
el('alertsToggle').onclick = async () => {
  if (alertsEnabled) {
    alertsEnabled = false;
  } else if (!supportsSystemAlerts) {
    toast('In-app alerts work here. Desktop notifications need localhost or HTTPS.');
    return;
  } else {
    // Browser permission is requested only after an explicit button click.
    let permission = Notification.permission;
    if (permission === 'default') permission = await Notification.requestPermission();
    if (permission !== 'granted') {
      toast('Desktop permission not granted. In-app message alerts still work.');
      return;
    }
    alertsEnabled = true;
  }
  try { localStorage.setItem('cc.desktopAlerts',alertsEnabled ? 'yes' : 'no'); } catch {}
  updateAlertsButton();
};
// PWA V1: show notifications via a registered Service Worker when supported
// (not Push API; the page still needs to be running for these events).
async function ccSystemNotify(title, body, tag, kind, id) {
  if (!alertsEnabled || !supportsSystemAlerts || Notification.permission !== 'granted' ||
      (!document.hidden && document.hasFocus())) return;
  try {
    if ('serviceWorker' in navigator) {
      const registration = await navigator.serviceWorker.getRegistration('/');
      if (registration && typeof registration.showNotification === 'function') {
        await registration.showNotification(title, {
          body, tag, icon:'/pwa-icon-192.png', badge:'/pwa-icon-192.png',
          silent:true, data:{kind, id:String(id||'')}
        });
        return;
      }
    }
    // Fallback works on many desktop browsers, but not reliably on mobile.
    const note = new Notification(title, {body, tag, silent:true, icon:'/pwa-icon-192.png'});
    note.onclick = () => {
      window.focus();
      if (session && kind === 'friend') select(String(id));
      if (session && kind === 'group') selectGroup(String(id));
      note.close();
    };
  } catch (error) { console.warn('ChatConnect OS notification unavailable:',error); }
}

function alertFriend(friendId,name) {
  toast('🔔 New private message from ' + name);
  document.title = '🔔 New message · ChatConnect';
  void ccSystemNotify('ChatConnect · New message',
    'You have a new private message.', 'cc-private-'+friendId, 'friend', friendId);
}
function currentDraftKey() { return groupTarget ? 'group:'+groupTarget : 'friend:'+target; }
function groupUnreadCount(id) {return Number((snapshot.groupUnread||{})[id]||0);}
async function acknowledgeCurrentGroup() {
  if(!session || !groupTarget || document.hidden || pendingGroupRead.has(groupTarget))return;
  const group=groupTarget;
  const newest=(snapshot.groupMessages||[]).filter(m=>m.groupId===group)
    .reduce((max,m)=>Math.max(max,Number(m.id)||0),0);
  if(!newest || (!groupUnreadCount(group) && newest<=(readGroups.get(group)||0)))return;
  pendingGroupRead.add(group);
  try {
    await api('groups/read',{group_id:group,up_to:String(newest)});
    readGroups.set(group,Math.max(newest,readGroups.get(group)||0));
    if(group===groupTarget && snapshot.groupUnread){snapshot.groupUnread[group]=0;renderGroups();}
  }catch(error){console.warn('Group read acknowledgement failed',error);}
  finally{pendingGroupRead.delete(group);}
}
function stopLiveUpdates() {
  ++liveGeneration;
  if (liveAbort) liveAbort.abort();
  liveAbort = null; liveCursor = null;
}
// Authenticated long-poll: no token in URL, and no message content in events.
// The 15-second fallback state refresh remains to recover after network failures.
async function startLiveUpdates() {
  stopLiveUpdates();
  const user = session, generation = liveGeneration;
  while (user && session === user && generation === liveGeneration) {
    const controller = new AbortController();
    liveAbort = controller;
    try {
      const headers = {Authorization:'Bearer ' + user.token};
      if (liveCursor !== null) headers['X-CC-Cursor'] = String(liveCursor);
      const response = await fetch('/api/events', {headers,cache:'no-store',signal:controller.signal});
      if (response.status === 401) { clearSession(); return; }
      if (!response.ok) throw new Error('Live event connection failed');
      const data = await response.json();
      if (session !== user || generation !== liveGeneration) return;
      liveCursor = Number(data.cursor);
      if (!Number.isSafeInteger(liveCursor) || liveCursor < 0) throw new Error('Invalid event cursor');
      if (data.changed) await refresh();
    } catch (error) {
      if (controller.signal.aborted || generation !== liveGeneration || session !== user) return;
      console.warn('ChatConnect live updates reconnecting:',error);
      await new Promise(resolve => setTimeout(resolve,2000));
    }
  }
}
const originalPageTitle = document.title;
function toast(text) {
  const notice = el('newMessageToast');
  notice.textContent = text;
  notice.hidden = false;
  clearTimeout(noticeTimeout);
  noticeTimeout = setTimeout(() => { notice.hidden = true; }, 5200);
}
// THEME STUDIO V1 — separate personal theme from two-way shared DM wallpaper.
const ccWallpaperCatalog=[{id:"doodle-blue",name:"Doodle Blue",description:"Playful \u00b7 Creative",accent:"#cbe6ff"},{id:"kawaii-sunshine",name:"Kawaii Sunshine",description:"Bright \u00b7 Cheerful",accent:"#ffe65c"},{id:"mint-blossom",name:"Mint Blossom",description:"Calm \u00b7 Natural",accent:"#b6d7bf"},{id:"cat-doodle",name:"Cat Doodle",description:"Cute \u00b7 Whimsical",accent:"#f8e9dd"},{id:"neon-love",name:"Neon Love",description:"Bold \u00b7 Romantic",accent:"#f740a5"},{id:"candy-clouds",name:"Candy Clouds",description:"Dreamy \u00b7 Pastel",accent:"#d8bbf6"},{id:"tropical-escape",name:"Tropical Escape",description:"Vibrant \u00b7 Refreshing",accent:"#58cdb7"},{id:"kitty-companions",name:"Kitty Companions",description:"Adorable \u00b7 Friendly",accent:"#bfe6ff"},{id:"floating-hearts",name:"Floating Hearts",description:"Sweet \u00b7 Loving",accent:"#fcd3de"},{id:"fluffy-bears",name:"Fluffy Bears",description:"Cozy \u00b7 Cute",accent:"#f8cddc"},{id:"pink-shoreline",name:"Pink Shoreline",description:"Serene \u00b7 Dreamy",accent:"#a7e2e4"}];
const ccWallpaperById=new Map(ccWallpaperCatalog.map(w=>[w.id,w]));
const ccThemeIdSet=new Set(['notepad','sunset',...ccWallpaperCatalog.map(w=>w.id)]);
const ccThemeDialog=el('ccThemeDialog');
let ccStudioScope='global',ccStudioSelected='notepad',ccStudioQuery='';
function ccStorageKey(){return 'cc.personal.theme.'+(session?.id||'guest');}
function ccLoadTheme(){
  let desired='notepad';
  try{
    desired=localStorage.getItem(ccStorageKey())||'notepad';
    if(session && !localStorage.getItem(ccStorageKey()) && !localStorage.getItem('cc.personal.legacy.migrated')){
      desired=localStorage.getItem('cc.theme')||'notepad';
      localStorage.setItem('cc.personal.legacy.migrated','yes');
    }
  }catch{}
  return ccThemeIdSet.has(desired)?desired:'notepad';
}
function ccCurrentTheme(){return document.body.dataset.ccTheme||'notepad';}
function updateTheme(theme) {
  const chosen=ccThemeIdSet.has(theme)?theme:'notepad';
  document.body.classList.toggle('theme-sunset',chosen==='sunset');
  document.body.dataset.ccTheme=chosen;
  const button=el('themeSwitch');
  button.textContent=chosen==='notepad'?'🌇 Sunset theme':'📒 Notepad theme';
  button.setAttribute('aria-label',chosen==='notepad'?'Switch to Sunset theme':'Switch to Notepad theme');
  try{if(session)localStorage.setItem(ccStorageKey(),chosen);}catch{}
  ccApplyWallpaper();
}
function ccWallpaperForThread(){
  if(groupTarget){
    const group=(snapshot.groups||[]).find(g=>g.id===groupTarget);
    return {shared:ccWallpaperById.has(group?.wallpaperId)?group.wallpaperId:'',isShared:!!group?.wallpaperId};
  }
  if(target){
    const id=(snapshot.dmWallpapers||{})[target]||'';
    return {shared:ccWallpaperById.has(id)?id:'',isShared:!!id};
  }
  return {shared:'',isShared:false};
}
function ccApplyWallpaper(){
  const feed=el('feed');if(!feed)return;
  const setting=ccWallpaperForThread();
  const personal=ccCurrentTheme();
  const wall=setting.shared||(ccWallpaperById.has(personal)?personal:'');
  if(wall)feed.dataset.ccWallpaper=wall;
  else delete feed.dataset.ccWallpaper;
  const badge=el('ccWallpaperStatus');
  if(badge){
    badge.hidden=!setting.isShared||(!target&&!groupTarget);
    badge.textContent=groupTarget?'🎨 Group wallpaper':'🎨 Shared wallpaper';
    badge.title=groupTarget?'The group owner sets this wallpaper for all members.':'Either friend can change this shared wallpaper. Only this conversation is affected.';
  }
}
function ccSelectedCurrentWallpaper(){
  if(ccStudioScope==='global')return ccCurrentTheme();
  const setting=ccWallpaperForThread();
  return setting.shared||'';
}
function ccThemeOpen(scope='global'){
  if(!session)return;
  if(scope==='chat' && !target && !groupTarget)scope='global';
  ccStudioScope=scope;
  ccStudioSelected=ccSelectedCurrentWallpaper();
  el('ccThemeSearch').value='';ccStudioQuery='';
  ccRenderStudio();
  if(!ccThemeDialog.open)ccThemeDialog.showModal();
}
function ccRenderStudio(){
  const isChat=ccStudioScope==='chat';
  el('ccThemeGlobal').setAttribute('aria-pressed',String(!isChat));
  el('ccThemeChat').setAttribute('aria-pressed',String(isChat));
  el('ccThemeChat').disabled=!target&&!groupTarget;
  el('ccThemeScopeNotice').textContent=isChat?
    (groupTarget?'The group owner chooses the wallpaper for every member. Your personal theme stays unchanged.':
      'You and your friend see the SAME wallpaper. Either of you can change it; your other chats keep your personal theme.'):
    'Choose YOUR app theme. It stays personal and does not change your friends’ apps.';
  const cards=el('ccThemeCards');cards.replaceChildren();
  const options=isChat?[...ccWallpaperCatalog]:[
    {id:'notepad',name:'Original Notepad',description:'Classic notebook',accent:'#f9e677'},
    {id:'sunset',name:'Cinematic Sunset',description:'Original dark orange',accent:'#ff9b53'},
    ...ccWallpaperCatalog];
  for(const item of options){
    if(ccStudioQuery && !(item.name+' '+item.description).toLowerCase().includes(ccStudioQuery))continue;
    const card=document.createElement('button');card.type='button';card.className='cc-theme-card';
    card.setAttribute('aria-pressed',String(item.id===ccStudioSelected));
    const photo=document.createElement('span');photo.className='cc-theme-thumbnail';
    if(ccWallpaperById.has(item.id)){
      const img=document.createElement('img');img.src='/theme-'+item.id+'.webp';img.alt='';img.loading='lazy';photo.append(img);
    }else{photo.classList.add('cc-theme-thumb-'+item.id);photo.textContent=item.id==='notepad'?'📒':'🌇';}
    const title=document.createElement('strong');title.textContent=item.name;
    const caption=document.createElement('small');caption.textContent=item.description;
    const dot=document.createElement('span');dot.className='cc-theme-dot';dot.style.backgroundColor=item.accent;
    card.append(photo,title,caption,dot);
    if(item.id===ccStudioSelected){const check=document.createElement('span');check.className='cc-theme-selected';check.textContent='✓';card.append(check);}
    card.onclick=()=>{ccStudioSelected=item.id;ccRenderStudio();};
    cards.append(card);
  }
  el('ccThemeEmpty').hidden=!!cards.childElementCount;
  const preview=el('ccThemePreview');
  const prev=ccWallpaperById.has(ccStudioSelected)?ccStudioSelected:'';
  if(prev)preview.dataset.ccWallpaper=prev;else delete preview.dataset.ccWallpaper;
  preview.classList.toggle('cc-theme-preview-sunset',ccStudioSelected==='sunset');
  el('ccThemePreviewLabel').textContent=ccWallpaperById.get(ccStudioSelected)?.name||
      (ccStudioSelected==='sunset'?'Cinematic Sunset':'Original Notepad');
  el('ccThemeApply').textContent=isChat?'✓ Apply to Both Sides of This Chat':'✓ Set My Global Theme';
  el('ccThemeReset').hidden=!isChat;
}
el('themeSwitch').onclick=()=>updateTheme(ccCurrentTheme()==='notepad'?'sunset':'notepad');
el('ccThemeStudioOpen').onclick=()=>ccThemeOpen('global');
el('ccChatWallpaperOpen').onclick=()=>ccThemeOpen('chat');
el('ccThemeClose').onclick=()=>ccThemeDialog.close();
el('ccThemeGlobal').onclick=()=>{ccStudioScope='global';ccStudioSelected=ccSelectedCurrentWallpaper();ccRenderStudio();};
el('ccThemeChat').onclick=()=>{if(target||groupTarget){ccStudioScope='chat';ccStudioSelected=ccSelectedCurrentWallpaper();ccRenderStudio();}};
el('ccThemeSearch').addEventListener('input',event=>{ccStudioQuery=event.target.value.trim().toLowerCase();ccRenderStudio();});
el('ccThemeApply').onclick=async()=>{
  const button=el('ccThemeApply'),error=el('ccThemeFeedback');error.textContent='';button.disabled=true;
  try{
    if(ccStudioScope==='global'){updateTheme(ccStudioSelected);}
    else if(target){await api('wallpaper/dm',{peer_id:target,wallpaper_id:ccStudioSelected});snapshot.dmWallpapers=snapshot.dmWallpapers||{};snapshot.dmWallpapers[target]=ccStudioSelected;ccApplyWallpaper();await refresh();}
    else if(groupTarget){await api('wallpaper/group',{group_id:groupTarget,wallpaper_id:ccStudioSelected});const g=(snapshot.groups||[]).find(g=>g.id===groupTarget);if(g)g.wallpaperId=ccStudioSelected;ccApplyWallpaper();await refresh();}
    ccThemeDialog.close();ccApplyWallpaper();
    toast(ccStudioScope==='global'?'🎨 Your personal theme saved!':'🎨 Shared chat wallpaper changed for everyone!');
  }catch(err){error.textContent=err.message;}
  finally{button.disabled=false;}
};
el('ccThemeReset').onclick=async()=>{
  const error=el('ccThemeFeedback');error.textContent='';
  try{
    if(target){await api('wallpaper/dm',{peer_id:target,wallpaper_id:''});snapshot.dmWallpapers=snapshot.dmWallpapers||{};snapshot.dmWallpapers[target]='';}
    else if(groupTarget){await api('wallpaper/group',{group_id:groupTarget,wallpaper_id:''});const g=(snapshot.groups||[]).find(g=>g.id===groupTarget);if(g)g.wallpaperId='';}
    await refresh();ccThemeDialog.close();ccApplyWallpaper();toast('Shared wallpaper cleared. Both users now see their own theme.');
  }catch(e){error.textContent=e.message;}
};
updateTheme(ccLoadTheme());
function detectNewMessages(data) {
  const newest = new Map();
  for (const row of data.messages || []) {
    if (row.to !== session.id || row.author === session.id) continue;
    const last = newest.get(row.author);
    if (!last || row.id > last.id) newest.set(row.author, row);
  }
  if (inboxInitialized) {
    for (const [friendId, row] of newest) {
      const previous = lastMessageByFriend.get(friendId)?.id || 0;
      if (row.id > previous && (friendId !== target || document.hidden || !document.hasFocus())) {
        const name = (data.contacts || []).find(person => person.id === friendId)?.name || row.name || 'Friend';
        // Muted friend: unread badges still update, but never produce a popup.
        if (!(data.contacts || []).some(person => person.id === friendId && person.muted))
          alertFriend(friendId,name);
      }
    }
  }
  lastMessageByFriend = newest.size ? newest : lastMessageByFriend;
  const latestGroups = new Map();
  for (const row of data.groupMessages || []) {
    if (row.author === session.id) continue;
    const prev = latestGroups.get(row.groupId);
    if (!prev || row.id > prev.id) latestGroups.set(row.groupId,row);
  }
  if (inboxInitialized) for (const [groupId,row] of latestGroups) {
    const previous = lastGroupMessages.get(groupId)?.id || 0;
    if (row.id > previous && (groupId !== groupTarget || document.hidden || !document.hasFocus())) {
      const group = (data.groups || []).find(g => g.id === groupId);
      if (group?.muted) continue; // Group mute suppresses alerts, never unread counts.
      toast('👥 New message in ' + (group?.name || 'your group'));
      document.title = '👥 Group message · ChatConnect';
      void ccSystemNotify('ChatConnect · Group message',
        'You have a new group message.', 'cc-group-'+groupId, 'group', groupId);
    }
  }
  if (latestGroups.size) lastGroupMessages=latestGroups;
  const groupInvites=new Set((data.groupInvites || []).map(inv=>String(inv.inviteId)));
  if (inboxInitialized && [...groupInvites].some(id=>!knownGroupInvites.has(id))) {
    toast('👥 New private group invitation!');
    void ccSystemNotify('ChatConnect · Group invitation',
      'You have a private group invitation.', 'cc-group-invite', 'invite', '');
  }
  knownGroupInvites=groupInvites;
  const requests = new Set((data.incoming || []).map(request => String(request.requestId)));
  if (inboxInitialized && [...requests].some(id => !knownRequests.has(id))) {
    toast('🤝 New friend request received!');
    document.title = '🤝 Friend request · ChatConnect';
  }
  knownRequests = requests;
  inboxInitialized = true;
}
async function acknowledgeCurrentChat() {
  // A chat is considered viewed when its tab is visible and the conversation
  // is selected. document.hasFocus() can stay false after the user clicks a
  // notification, interacts with browser controls, or switches windows.
  if (!session || !target || document.hidden) return;
  const reader = session;
  const from = target;
  const newest = currentMessages(from)
    .filter(m => m.author === from && m.to === reader.id)
    .reduce((id, m) => Math.max(id, Number(m.id) || 0), 0);
  const unread = unreadCount(from);
  if (!newest || pendingRead.has(from)) return;
  // A stale /state response can resurrect the unread badge after an earlier
  // successful /read. Retry while the server *still reports* unread messages.
  if (!unread && newest <= (readAcknowledged.get(from) || 0)) return;
  const now = Date.now();
  if (now - (readRetryAt.get(from) || 0) < 1500) return;
  readRetryAt.set(from, now);
  pendingRead.add(from);
  try {
    await api('read', {from, up_to: String(newest)}, reader);
    readAcknowledged.set(from, Math.max(newest, readAcknowledged.get(from) || 0));
    if (session === reader && target === from && snapshot.unread) {
      // The server accepted the read marker. The next state poll reconciles
      // the badge with the persisted PostgreSQL count.
      snapshot.unread[from] = 0;
      render();
    }
  } catch (error) {
    // A failed request must not clear the unread count. Polling retries it.
    console.warn('ChatConnect: could not update read status', error);
  } finally {
    pendingRead.delete(from);
  }
}
// A message is Delivered only after the receiver's authenticated browser has
// actually fetched the inbox and acknowledged receipt. Mere server storage is Sent.
function acknowledgeDeliveries() {
  if (!session || !snapshot.messages) return;
  const viewer = session;
  const idsBySender = new Map();
  for (const message of snapshot.messages) {
    if (message.to !== viewer.id || message.author === viewer.id) continue;
    const id = Number(message.id) || 0;
    if (id <= (deliveredAcknowledged.get(message.author)||0)) continue;
    if (!idsBySender.has(message.author)) idsBySender.set(message.author, []);
    idsBySender.get(message.author).push(id);
  }
  for (const [from, ids] of idsBySender) {
    if (pendingDelivered.has(from) || !ids.length) continue;
    const newest = Math.max(...ids);
    pendingDelivered.add(from);
    api('delivered', {from,ids:ids.join(',')}, viewer).then(() => {
      if (session===viewer) deliveredAcknowledged.set(from, Math.max(newest, deliveredAcknowledged.get(from)||0));
    }).catch(error => {
      console.warn('ChatConnect: delivery acknowledgement failed', error);
    }).finally(() => pendingDelivered.delete(from));
  }
}
window.addEventListener('focus', () => {document.title = originalPageTitle; refresh(); acknowledgeCurrentChat(); acknowledgeCurrentGroup();});
document.addEventListener('visibilitychange', () => {if (!document.hidden) {document.title = originalPageTitle; refresh(); acknowledgeCurrentChat(); acknowledgeCurrentGroup();}});

async function api(path, data, auth = session) {
  const options = {method: data ? 'POST' : 'GET', headers: {}, cache: 'no-store'};
  if (auth) options.headers.Authorization = 'Bearer ' + auth.token;
  if (data) options.body = new URLSearchParams(data);
  const response = await fetch('/api/' + path, options);
  const result = await response.json();
  if (!response.ok) {
    const error = new Error(result.error || 'Request failed');
    error.status = response.status;
    throw error;
  }
  return result;
}
// RELIABLE TEXT OUTBOX V1
// Queue is scoped to the signed-in user and persisted only in this browser tab's
// sessionStorage. It survives reloads while the tab session exists. It is NOT
// background sync: the page must be open, authenticated and connected to retry.
let ccOutbox = [], ccOutboxUser = '', ccOutboxSending = false, ccOutboxTimer = null;
const ccOutboxPanel = document.createElement('section');
ccOutboxPanel.className = 'cc-outbox';
ccOutboxPanel.id = 'ccOutboxPanel';
ccOutboxPanel.setAttribute('role','status');
ccOutboxPanel.setAttribute('aria-live','polite');
ccOutboxPanel.hidden = true;
el('compose').before(ccOutboxPanel);
function ccOutboxKey(userId) {return 'cc.pending-text.v1.' + String(userId);}
function ccRandomRef() {
  if (!globalThis.crypto || !crypto.getRandomValues) throw new Error('Secure random IDs not supported.');
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, b => b.toString(16).padStart(2,'0')).join('');
}
function ccIsOutboxEntry(value) {
  return value && /^[a-f0-9]{32}$/.test(value.ref) &&
    ['dm','group'].includes(value.kind) && typeof value.target==='string' &&
    value.target.length<=32 && typeof value.text==='string' &&
    new TextEncoder().encode(value.text).length<=2000 &&
    typeof value.reply==='string' && value.reply.length<=19 &&
    ['waiting','sending','failed'].includes(value.state);
}
function ccLoadOutbox() {
  clearTimeout(ccOutboxTimer);
  ccOutbox=[];ccOutboxUser=session?.id || '';
  if (!ccOutboxUser) {ccDrawOutbox();return;}
  try {
    const rows=JSON.parse(sessionStorage.getItem(ccOutboxKey(ccOutboxUser))||'[]');
    if (Array.isArray(rows)) ccOutbox=rows.filter(ccIsOutboxEntry).slice(0,30).map(row=>({
      ref:row.ref,kind:row.kind,target:row.target,text:row.text,reply:row.reply,
      state:row.state==='sending'?'waiting':row.state,
      error:String(row.error||'').slice(0,180),created:Number(row.created)||Date.now(),attempts:Number(row.attempts)||0
    }));
  }catch(e){console.warn('Could not load pending text messages',e);}
  ccDrawOutbox();
}
function ccSaveOutbox() {
  if (!ccOutboxUser) return false;
  try {sessionStorage.setItem(ccOutboxKey(ccOutboxUser),JSON.stringify(ccOutbox));return true;}
  catch(e){console.error('Could not save pending messages',e);return false;}
}
function ccScheduleOutbox(delay=0) {
  clearTimeout(ccOutboxTimer);
  if(session && ccOutbox.length)ccOutboxTimer=setTimeout(ccDrainOutbox,delay);
}
function ccDrawOutbox() {
  const panel=ccOutboxPanel;panel.replaceChildren();
  const mine=ccOutbox.filter(row=>row.kind===(groupTarget?'group':'dm') &&
    row.target===(groupTarget||target||''));
  if(!session || !ccOutbox.length){panel.hidden=true;return;}
  panel.hidden=false;
  const top=document.createElement('div');top.className='cc-outbox-head';
  const summary=document.createElement('strong');
  summary.textContent='📤 Pending text messages: '+ccOutbox.length;
  const retry=document.createElement('button');retry.type='button';retry.textContent='↻ Retry now';
  retry.onclick=()=>{for(const row of ccOutbox)if(row.state==='failed')row.state='waiting';ccSaveOutbox();ccDrawOutbox();ccScheduleOutbox(0);};
  top.append(summary,retry);panel.append(top);
  if(!mine.length){const text=document.createElement('small');text.textContent='Queued in other chats. Keep this tab open to send.';panel.append(text);return;}
  for(const item of mine.slice(0,5)) {
    const row=document.createElement('div');row.className='cc-outbox-row';
    const content=document.createElement('span');content.textContent=item.text.slice(0,100);
    const status=document.createElement('small');
    status.textContent=item.state==='sending'?'⏳ Sending':item.state==='failed'?'⚠️ Needs attention':'📡 Queued for reconnect';
    status.title=item.error||'';
    row.append(content,status);
    if(item.state!=='sending'){
      const discard=document.createElement('button');discard.type='button';discard.textContent='✕';
      discard.title='Stop retrying (message may already have reached the server)';
      discard.onclick=()=>{
        if(!window.confirm('Stop retrying this message? It may already be delivered.'))return;
        ccOutbox=ccOutbox.filter(entry=>entry.ref!==item.ref);
        ccSaveOutbox();ccDrawOutbox();
      };
      row.append(discard);
    }
    panel.append(row);
  }
}
function ccQueueText(text,recipient,group,reply) {
  if(!session)throw new Error('Sign in to send a message.');
  if(ccOutbox.length>=30)throw new Error('Offline outbox is full (30 messages). Reconnect or retry first.');
  const item={ref:ccRandomRef(),kind:group?'group':'dm',target:group||recipient||'',text,
    reply:reply||'',state:'waiting',error:'',created:Date.now(),attempts:0};
  ccOutbox.push(item);
  if(!ccSaveOutbox()){ccOutbox.pop();throw new Error('Cannot save offline messages in this tab. Message kept in compose box.');}
  ccDrawOutbox();ccScheduleOutbox(0);
  return item;
}
async function ccDrainOutbox() {
  if(ccOutboxSending || !session || session.id!==ccOutboxUser || !ccOutbox.length)return;
  if(navigator.onLine===false){ccScheduleOutbox(8000);return;}
  const item=ccOutbox.find(row=>row.state==='waiting');
  if(!item)return;
  ccOutboxSending=true;
  item.state='sending';item.attempts++;ccSaveOutbox();ccDrawOutbox();
  const current=session;
  try {
    if(item.kind==='group')await api('groups/send',{
      group_id:item.target,text:item.text,reply_to:item.reply,client_ref:item.ref
    },current);
    else await api('send',{
      to:item.target,text:item.text,reply_to:item.reply,client_ref:item.ref
    },current);
    if(session===current && ccOutboxUser===current.id) {
      ccOutbox=ccOutbox.filter(row=>row.ref!==item.ref);
      ccSaveOutbox();ccDrawOutbox();
      lastFeed='';await refresh();
    }
  }catch(error){
    if(session===current && ccOutbox.includes(item)){
      item.error=error.message||'Connection unavailable';
      const transient=!error.status || error.status===408 || error.status===429 || error.status>=500;
      item.state=transient?'waiting':'failed';
      ccSaveOutbox();ccDrawOutbox();
      if(transient){
        const retryDelay=Math.min(30000,1500*Math.pow(2,Math.min(5,item.attempts)));
        ccOutboxSending=false;ccScheduleOutbox(retryDelay);return;
      }
      el('sendError').textContent='Queued message could not be sent: '+item.error;
    }
  }finally{ccOutboxSending=false;}
  if(session===current && ccOutbox.some(row=>row.state==='waiting'))ccScheduleOutbox(250);
}
window.addEventListener('online',()=>{refresh();ccScheduleOutbox(0);});
window.addEventListener('focus',()=>{if(ccOutbox.length)ccScheduleOutbox(0);});

// Photo/File Sharing: private DM and groups, up to 300 MiB.
// Uploads are 512 KiB chunks; large files never enter a single JSON/SQL request.
const MAX_MEDIA_BYTES = 300*1024*1024;
const MEDIA_CHUNK_SIZE = 512*1024;
const photoPreviewCache = new Map();
const photoPreviewLoading = new Map();
const photoPreviewLimit = 10;
function clearAttachmentCache() {
  for(const url of photoPreviewCache.values()) URL.revokeObjectURL(url);
  photoPreviewCache.clear();photoPreviewLoading.clear();
}
async function photoBlobUrl(fileId) {
  const id=String(fileId);
  if(photoPreviewCache.has(id))return photoPreviewCache.get(id);
  if(photoPreviewLoading.has(id))return photoPreviewLoading.get(id);
  const viewer=session;
  const task=(async()=>{
    const res=await fetch('/api/attachments/preview',{
      method:'POST',headers:{Authorization:'Bearer '+viewer.token},
      cache:'no-store',body:new URLSearchParams({id})
    });
    if(!res.ok){let error='Photo preview unavailable.';
      try {error=(await res.json()).error||error;}catch{}throw new Error(error);}
    if(session!==viewer)throw new Error('Account changed.');
    const blob=await res.blob();
    if(session!==viewer)throw new Error('Account changed.');
    const url=URL.createObjectURL(blob);
    if(photoPreviewCache.size>=photoPreviewLimit){
      const oldest=photoPreviewCache.keys().next().value;
      URL.revokeObjectURL(photoPreviewCache.get(oldest));photoPreviewCache.delete(oldest);
    }
    photoPreviewCache.set(id,url);return url;
  })();
  photoPreviewLoading.set(id,task);
  try{return await task;}finally{photoPreviewLoading.delete(id);}
}
const mediaObserver=typeof IntersectionObserver==='function' ? new IntersectionObserver(entries=>{
  for(const entry of entries)if(entry.isIntersecting){
    const pic=entry.target;mediaObserver.unobserve(pic);
    photoBlobUrl(pic.dataset.fileId).then(url=>{if(pic.isConnected)pic.src=url;})
      .catch(()=>{if(pic.isConnected)pic.alt='Photo preview unavailable';});
  }
},{rootMargin:'140px'}):null;
function requestNativeAttachmentDownload(fileId) {
  // Native browser download: stream from server straight into browser's download
  // manager, instead of building a 300 MB JS Blob. No token in a URL.
  if(!session)throw new Error('Sign in to download this file.');
  let iframe=document.getElementById('cc-file-download-frame');
  if(!iframe){iframe=document.createElement('iframe');iframe.id='cc-file-download-frame';
    iframe.name='cc-file-download-frame';iframe.hidden=true;
    iframe.title='Private attachment download';document.body.append(iframe);}
  const form=document.createElement('form');form.method='POST';
  form.action='/api/attachments/download';form.target='cc-file-download-frame';form.hidden=true;
  for(const [name,value] of Object.entries({id:String(fileId),download_token:session.token})){
    const input=document.createElement('input');input.type='hidden';input.name=name;input.value=value;
    form.append(input);
  }
  document.body.append(form);form.submit();form.remove();
}
// Click-to-expand secure photo viewer. No attachment IDs or auth tokens in URLs.
// Reuses the authenticated preview endpoint already used by normal DM and group chats.
let ccPhotoSelected=null;
let ccPhotoZoom=1;
let ccPhotoReturnFocus=null;
const ccPhotoDialog=el('ccPhotoDialog');
function ccUpdatePhotoZoom(){
  const image=el('ccPhotoImage');
  image.style.width=Math.round(ccPhotoZoom*100)+'%';
  el('ccPhotoZoomLabel').textContent=Math.round(ccPhotoZoom*100)+'%';
  el('ccPhotoZoomIn').disabled=ccPhotoZoom>=3;
  el('ccPhotoZoomOut').disabled=ccPhotoZoom<=0.5;
}
async function ccOpenPhoto(attachment){
  if(!session || !attachment)return;
  ccPhotoReturnFocus=document.activeElement;
  ccPhotoSelected={id:String(attachment.id),name:String(attachment.name||'Photo'),session};
  ccPhotoZoom=1;
  ccUpdatePhotoZoom();
  const photo=el('ccPhotoImage');
  photo.removeAttribute('src');
  photo.alt='Expanded photo: '+ccPhotoSelected.name;
  photo.hidden=true;
  el('ccPhotoName').textContent=ccPhotoSelected.name;
  el('ccPhotoFeedback').textContent='Loading private photo…';
  if(!ccPhotoDialog.open)ccPhotoDialog.showModal();
  el('ccPhotoClose').focus();
  try{
    const expected=ccPhotoSelected;
    const url=await photoBlobUrl(expected.id);
    // Do not show stale images when closing, switching accounts or opening another file.
    if(!ccPhotoDialog.open || ccPhotoSelected!==expected || session!==expected.session)return;
    photo.src=url;
    photo.hidden=false;
    el('ccPhotoFeedback').textContent='Scroll to explore • zoom for details';
  }catch(error){
    if(ccPhotoDialog.open && ccPhotoSelected && String(attachment.id)===ccPhotoSelected.id){
      el('ccPhotoFeedback').textContent=error.message||'Photo cannot be displayed.';
    }
  }
}
function ccClosePhoto(){if(ccPhotoDialog.open)ccPhotoDialog.close();}
ccPhotoDialog.addEventListener('close',()=>{
  ccPhotoSelected=null;
  el('ccPhotoImage').removeAttribute('src');
  el('ccPhotoImage').hidden=true;
  const focus=ccPhotoReturnFocus;
  ccPhotoReturnFocus=null;
  if(focus && focus.isConnected)focus.focus();
});
ccPhotoDialog.addEventListener('click',event=>{
  if(event.target===ccPhotoDialog)ccClosePhoto();
});
el('ccPhotoClose').onclick=ccClosePhoto;
el('ccPhotoZoomIn').onclick=()=>{ccPhotoZoom=Math.min(3,Math.round((ccPhotoZoom+0.25)*100)/100);ccUpdatePhotoZoom();};
el('ccPhotoZoomOut').onclick=()=>{ccPhotoZoom=Math.max(0.5,Math.round((ccPhotoZoom-0.25)*100)/100);ccUpdatePhotoZoom();};
el('ccPhotoZoomReset').onclick=()=>{ccPhotoZoom=1;ccUpdatePhotoZoom();};
el('ccPhotoDownload').onclick=()=>{
  if(!ccPhotoSelected || !session || session!==ccPhotoSelected.session)return;
  try{requestNativeAttachmentDownload(ccPhotoSelected.id);}
  catch(error){el('ccPhotoFeedback').textContent=error.message;}
};
function renderAttachment(card,attachment) {
  if(!attachment)return;
  if(String(attachment.mime||'').startsWith('audio/') &&
      typeof window.ccRenderVoiceAttachment==='function'){
    window.ccRenderVoiceAttachment(card,attachment);return;
  }
  const box=document.createElement('div');box.className='cc-attachment-card';
  const image=String(attachment.mime||'').startsWith('image/');
  const sizeBytes=Number(attachment.size||0);
  if(image && sizeBytes<=8*1024*1024){
    const pic=document.createElement('img');pic.className='cc-attachment-photo';
    pic.alt='Photo: '+attachment.name;pic.loading='lazy';pic.dataset.fileId=String(attachment.id);
    pic.referrerPolicy='no-referrer';
    pic.tabIndex=0;pic.setAttribute('role','button');
    pic.setAttribute('aria-label','Open photo '+attachment.name+' in the full-size viewer');
    pic.title='Click to open full photo';
    pic.onclick=()=>ccOpenPhoto(attachment);
    pic.onkeydown=event=>{
      if(event.key==='Enter' || event.key===' '){event.preventDefault();ccOpenPhoto(attachment);}
    };
    box.append(pic);
    if(mediaObserver)mediaObserver.observe(pic);
    else photoBlobUrl(attachment.id).then(url=>{if(pic.isConnected)pic.src=url;}).catch(()=>{});
  } else if(image) {
    const info=document.createElement('p');info.className='cc-attachment-preview-note';
    info.textContent='Large photo — download to view (preview limited to 8 MB).';box.append(info);
  }
  const line=document.createElement('div');line.className='cc-attachment-info';
  const label=document.createElement('span');label.className='cc-attachment-name';
  label.textContent=(image?'📷 ':'📁 ')+attachment.name;
  const size=document.createElement('small');
  size.textContent=sizeBytes>=1024*1024?(sizeBytes/1024/1024).toFixed(1)+' MB':(sizeBytes/1024).toFixed(0)+' KB';
  const download=document.createElement('button');download.type='button';
  download.className='cc-attachment-download';download.textContent='↓ Download';
  download.onclick=()=>{try{requestNativeAttachmentDownload(attachment.id);}
    catch(error){el('sendError').textContent=error.message;}};
  line.append(label,size,download);box.append(line);card.append(box);
}
function bufferToBase64(buffer) {
  const bytes=new Uint8Array(buffer);
  let text='';
  // bounded <= 512KiB; avoid stack overflow from spread/apply
  for(let n=0;n<bytes.length;n+=8192)
    text+=String.fromCharCode(...bytes.subarray(n,Math.min(n+8192,bytes.length)));
  // Base64url minimizes unnecessary URLSearchParams percent encoding.
  return btoa(text).replaceAll('+','-').replaceAll('/','_');
}
let activeMediaUpload=null;
el('attachButton').onclick=()=>{
  if(!session){el('sendError').textContent='Please sign in first.';return;}
  if(!target&&!groupTarget){el('sendError').textContent='Choose a connected friend or a private group.';return;}
  if(activeMediaUpload){el('sendError').textContent='Please finish the current upload.';return;}
  el('attachmentPicker').click();
};
async function ccUploadMediaFile(file,{voice=false}={}){
  if(!file)return false;
  const recipient=target,group=groupTarget,viewer=session;
  const btn=voice?el('ccVoiceSend'):el('attachButton');
  if(!viewer||(!recipient&&!group)){el('sendError').textContent='Select a connected friend or group first.';return false;}
  if(activeMediaUpload){el('sendError').textContent='Finish the current upload first.';return false;}
  if(file.size<1||file.size>MAX_MEDIA_BYTES){
    el('sendError').textContent=voice?'Voice notes must be 300 MiB or less.':'Choose a file up to 300 MiB.';return false;
  }
  if(!/\.(jpe?g|png|webp|pdf|txt|docx|xlsx|pptx|webm|ogg|m4a)$/i.test(file.name)){
    el('sendError').textContent='Unsupported file type.';return false;
  }
  btn.disabled=true;if(!voice)btn.textContent='⏳';
  el('sendError').textContent='Preparing '+file.name+'…';
  let uploadId='';activeMediaUpload={viewer,fileName:file.name};
  try{
    const started=await api('attachments/init',{
      scope:group?'group':'dm',to:recipient,group_id:group,name:file.name,size:String(file.size)
    },viewer);
    uploadId=started.uploadId;
    let offset=0;
    while(offset<file.size){
      if(session!==viewer || target!==recipient || groupTarget!==group)
        throw new Error('Account/chat changed; upload cancelled.');
      const end=Math.min(offset+MEDIA_CHUNK_SIZE,file.size);
      const chunk=await file.slice(offset,end).arrayBuffer();
      const result=await api('attachments/chunk',{
        upload_id:uploadId,offset:String(offset),data:bufferToBase64(chunk)
      },viewer);
      offset=Number(result.received);
      const percent=Math.floor(offset/file.size*100);
      if(!voice)btn.textContent='⏳'+percent+'%';
      el('sendError').textContent='Uploading '+(voice?'voice note':file.name)+' — '+percent+'%';
    }
    await api('attachments/complete',{upload_id:uploadId},viewer);
    el('sendError').textContent='✅ '+(voice?'Voice note':'Attachment')+' sent to '+(group?'group':'friend')+'!';
    await refresh();return true;
  }catch(error){el('sendError').textContent=error.message;
    if(uploadId){try{await api('attachments/abort',{upload_id:uploadId},viewer);}catch{}}
    return false;
  }finally{
    btn.disabled=false;if(!voice)btn.textContent='📎';activeMediaUpload=null;
  }
}
el('attachmentPicker').onchange=async()=>{
  const picker=el('attachmentPicker'),file=picker.files?.[0];
  if(file)await ccUploadMediaFile(file);
  picker.value='';
};
// Badge displays total unread (only when the browser supports App Badging).
function ccUpdateAppBadge() {
  if (!('setAppBadge' in navigator)) return;
  const unread = session ? Object.values(snapshot.unread||{}).reduce((sum,n)=>sum+Math.max(0,Number(n)||0),0) : 0;
  const groups = session ? Object.values(snapshot.groupUnread||{}).reduce((sum,n)=>sum+Math.max(0,Number(n)||0),0) : 0;
  const requests = session ? (snapshot.incoming||[]).length + (snapshot.groupInvites||[]).length : 0;
  const count = Math.min(999,unread+groups+requests);
  const task = count ? navigator.setAppBadge(count) : navigator.clearAppBadge?.();
  if (task && typeof task.catch==='function') task.catch(()=>{});
}

function clearSession() {
  window.ccMediaGalleryClose?.();
  window.ccCallsCleanup?.();
  window.ccVoiceCleanup?.();window.ccEmojiClose?.();
  ccClosePhoto();
  ccCloseSearch(); ccSearchPinned=null; ccSearchFocusId='';
  clearAttachmentCache();
  clearInterval(timer);
  clearTimeout(typingTimer);
  lastTypingRequest=0;
  stopLiveUpdates();
  clearTimeout(ccOutboxTimer);ccOutbox=[];ccOutboxUser='';ccDrawOutbox();
  session = null;
  window.ccCloseMobileDrawer?.();
  ccUpdateAppBadge();
  updateTheme('notepad');
  sessionStorage.removeItem('cc.session');
  peerNames.clear(); drafts.clear(); groupTarget='';ccClearReply(); knownGroupInvites.clear(); lastGroupMessages.clear(); readGroups.clear(); pendingGroupRead.clear(); readAcknowledged.clear(); pendingRead.clear(); readRetryAt.clear(); deliveredAcknowledged.clear(); pendingDelivered.clear(); lastMessageByFriend.clear(); inboxInitialized=false; knownRequests.clear(); clearTimeout(noticeTimeout); el('newMessageToast').hidden=true; document.title=originalPageTitle;
  target = ''; lastFeed = ''; editingMessageId = null; snapshot = {users:[],contacts:[],messages:[],typing:[],incoming:[],outgoing:[],blocked:[],unread:{},groups:[],groupInvites:[],groupMessages:[],groupUnread:{}};
  el('draft').value = ''; el('password').value = '';
  el('chat').hidden = true; el('entry').hidden = false;
}
function display() {
  updateTheme(ccLoadTheme());
  el('entry').hidden = true; el('chat').hidden = false;
  el('me').textContent = session.name; // Unique username remains unchanged by profile edits.
  el('myPublicId').textContent = session.publicId || 'Loading…';
  clearInterval(timer);
  ccLoadOutbox();
  refresh(); timer = setInterval(refresh, 15000); startLiveUpdates();
  ccScheduleOutbox(600);
}
function currentMessages(id = target) {
  const rows = groupTarget ? (snapshot.groupMessages||[]).filter(m=>m.groupId===groupTarget) :
    snapshot.messages.filter(m => id ? !!m.to && (m.author === id || m.to === id) : !m.to);
  // /api/state intentionally returns recent messages only; the authenticated search
  // endpoint can retrieve older hits. Keep one selected old hit until chat switch.
  const pin=ccSearchPinned;
  if(!pin || ((groupTarget ? 'group' : id ? 'friend' : 'notes')!==pin.kind) ||
     (groupTarget||id||'')!==pin.threadId || rows.some(m=>String(m.id)===String(pin.row.id))) return rows;
  return [...rows,pin.row].sort((a,b)=>Number(a.time)-Number(b.time)||Number(a.id)-Number(b.id));
}
function unreadCount(id) {
  if (!id || !snapshot.unread) return 0;
  return Number(snapshot.unread[id] || 0);
}
function renderRequests() {
  const incoming = el('incomingRequests'), outgoing = el('outgoingRequests');
  incoming.replaceChildren(); outgoing.replaceChildren();
  const received = snapshot.incoming || [], sent = snapshot.outgoing || [];
  el('requestBadge').textContent = String(received.length);
  el('requestBadge').hidden = received.length === 0;
  for (const request of received) {
    const row = document.createElement('div'); row.className = 'request-row';
    const name = document.createElement('p');
    name.textContent = request.name + ' · ' + request.publicId;
    const buttons = document.createElement('div'); buttons.className = 'request-actions';
    const yes = document.createElement('button'); yes.type = 'button'; yes.textContent = 'Accept ✓'; yes.className = 'request-yes';
    const no = document.createElement('button'); no.type = 'button'; no.textContent = 'Decline';
    async function decide(accept) {
      yes.disabled = true; no.disabled = true;
      try {
        await api(accept ? 'requests/accept' : 'requests/reject', {request_id: String(request.requestId)});
        el('connectFeedback').textContent = accept ? 'Connected! You can chat now.' : 'Request declined.';
        await refresh();
      } catch (error) { el('connectFeedback').textContent = error.message; }
      finally { yes.disabled = false; no.disabled = false; }
    }
    yes.onclick = () => decide(true); no.onclick = () => decide(false);
    const block = document.createElement('button'); block.type = 'button';
    block.textContent = '🚫 Block'; block.className = 'request-block';
    block.onclick = async () => {
      if (!confirm('Block @' + request.name + '? This will reject their request and prevent future contact.')) return;
      block.disabled = true;
      try {
        await api('friends/block',{public_id:request.publicId});
        toast('Account blocked.'); await refresh();
      } catch (error) { el('connectFeedback').textContent = error.message; }
      finally { block.disabled = false; }
    };
    buttons.append(yes, no, block); row.append(name, buttons); incoming.append(row);
  }
  if (!received.length) {
    const p = document.createElement('p'); p.className = 'request-hint'; p.textContent = 'No incoming requests'; incoming.append(p);
  }
  for (const request of sent) {
    const p = document.createElement('p'); p.className = 'pending';
    p.textContent = '⏳ ' + request.name + ' · Waiting for acceptance'; outgoing.append(p);
  }
}
el('connectForm').onsubmit = async event => {
  event.preventDefault();
  const submit = el('connectSubmit'); submit.disabled = true;
  el('connectFeedback').textContent = '';
  try {
    const result = await api('connect', {public_id: el('friendId').value.trim().toUpperCase()});
    el('connectFeedback').textContent = result.status === 'connected'
      ? 'Already connected with ' + result.name + ' ✓'
      : result.status === 'incoming'
        ? result.name + ' has already requested you. Accept their request below.'
        : 'Request sent to ' + result.name + '. Once accepted, your connection is saved.';
    el('friendId').value = '';
    await refresh();
  } catch (error) { el('connectFeedback').textContent = error.message; }
  finally { submit.disabled = false; }
};
el('copyId').onclick = async () => {
  if (!session || !session.publicId) return;
  try { await navigator.clipboard.writeText(session.publicId); el('connectFeedback').textContent = 'Your ID copied. Share it with your friend.'; }
  catch { el('connectFeedback').textContent = 'Copy this ID manually: ' + session.publicId; }
};
// Profile information is server-backed, not editable user IDs or usernames.
const avatarThemes = new Set(['cobalt','sunset','mint','violet','rose']);
function setAvatar(node, label, theme) {
  const safeTheme = avatarThemes.has(theme) ? theme : 'cobalt';
  node.className = 'cc-avatar theme-' + safeTheme;
  node.textContent = Array.from((label || 'CC').trim()).slice(0,2).join('').toUpperCase();
}
async function showMyProfile() {
  if (!session) return;
  el('profileFeedback').textContent = '';
  try {
    const me = await api('profile');
    el('profileUsername').textContent = '@' + me.username;
    el('profilePermanentId').textContent = me.publicId;
    el('profileDisplayName').value = me.displayName || me.username;
    el('profileBio').value = me.bio || '';
    el('profileColor').value = avatarThemes.has(me.avatarTheme) ? me.avatarTheme : 'cobalt';
    updateProfilePreview();
    el('profileDialog').showModal();
  } catch (error) { toast('Could not open profile: ' + error.message); }
}
function updateProfilePreview() {
  const name = el('profileDisplayName').value.trim() || session?.name || 'CC';
  el('profilePreviewName').textContent = name;
  setAvatar(el('profileAvatar'), name, el('profileColor').value);
}
el('profileOpen').onclick = showMyProfile;
el('profileClose').onclick = () => el('profileDialog').close();
el('profileDisplayName').addEventListener('input', updateProfilePreview);
el('profileColor').addEventListener('change', updateProfilePreview);
el('profileForm').onsubmit = async event => {
  event.preventDefault();
  const name = el('profileDisplayName').value.trim();
  const bio = el('profileBio').value.trim();
  const avatar = el('profileColor').value;
  const encoder = new TextEncoder();
  if (encoder.encode(name).length < 2 || encoder.encode(name).length > 48 ||
      encoder.encode(bio).length > 160) {
    el('profileFeedback').textContent = 'Name: 2–48 bytes; bio: max 160 bytes.';
    return;
  }
  const save = el('profileSave'); save.disabled = true;
  try {
    await api('profile', {display_name:name, bio, avatar_theme:avatar});
    el('profileFeedback').textContent = 'Saved ✓';
    el('profileDialog').close();
    await refresh();
  } catch (error) { el('profileFeedback').textContent = error.message; }
  finally {save.disabled = false;}
};
let safetyFriendId = '';
function safetyContact() {return (snapshot.contacts || []).find(person => person.id === safetyFriendId);}
function updateSafetyPanel() {
  const friend = safetyContact();
  if (!friend) {el('friendProfileDialog').close(); return;}
  el('friendMute').textContent = friend.muted ? '🔔 Unmute alerts' : '🔕 Mute alerts';
  el('friendMute').setAttribute('aria-pressed',String(!!friend.muted));
}
async function updateFriendSafety(action,confirmText='') {
  const friend = safetyContact();
  if (!friend || !session) return;
  if (confirmText && !window.confirm(confirmText)) return;
  const buttons = ['friendMute','friendRemove','friendBlock'].map(el);
  buttons.forEach(button => button.disabled = true);
  el('friendSafetyFeedback').textContent = '';
  try {
    await api('friends/' + action,{public_id:friend.publicId});
    if (action==='block' || action==='remove') {
      el('friendProfileDialog').close();
      if (target === friend.id) {target=''; lastFeed=''; el('draft').value='';}
      toast(action==='block' ? 'Account blocked. Future messages and requests are stopped.' :
        'Friend removed. Reconnect using IDs to chat again.');
    } else {
      toast(action==='mute' ? 'Muted alerts from ' + (friend.displayName||friend.name) : 'Alerts restored.');
    }
    await refresh();
    if (action==='mute' || action==='unmute') updateSafetyPanel();
  } catch (error) { el('friendSafetyFeedback').textContent = error.message; }
  finally { buttons.forEach(button => button.disabled = false); }
}
el('friendMute').onclick = () => {
  const friend = safetyContact();
  if (friend) updateFriendSafety(friend.muted ? 'unmute':'mute');
};
el('friendRemove').onclick = () => {
  const friend=safetyContact(); if(!friend)return;
  updateFriendSafety('remove','Remove @'+friend.name+' from friends? You will both need to reconnect to chat again.');
};
el('friendBlock').onclick = () => {
  const friend=safetyContact(); if(!friend)return;
  updateFriendSafety('block','Block @'+friend.name+'? Connection will end, messages and requests will be blocked.');
};
el('friendProfileOpen').onclick = () => {
  if (!session || !target) return;
  // Friend profile is sourced only from authenticated, accepted-friend state.
  const friend = (snapshot.contacts || []).find(person => person.id === target);
  if (!friend) return;
  safetyFriendId = friend.id;
  el('friendSafetyFeedback').textContent = '';
  updateSafetyPanel();
  const name = friend.displayName || friend.name;
  el('friendProfileDisplayName').textContent = name;
  el('friendProfileUsername').textContent = '@' + friend.name;
  el('friendProfileId').textContent = friend.publicId;
  el('friendProfileBio').textContent = friend.bio || 'No bio yet.';
  setAvatar(el('friendProfileAvatar'), name, friend.avatarTheme);
  el('friendProfileDialog').showModal();
};
el('friendProfileClose').onclick = () => el('friendProfileDialog').close();

// PRIVATE GROUPS: Group identity and membership come only from authenticated APIs.
// A group invitation must be accepted before the group appears in Chats.
function selectGroup(id) {
  if(groupTarget!==id)window.ccVoiceCleanup?.();
  window.ccEmojiClose?.();
  if(groupTarget===id){acknowledgeCurrentGroup();return;}
  ccSearchPinned=null;ccSearchFocusId='';
  drafts.set(currentDraftKey(),el('draft').value);
  ccStopTyping();
  target='';groupTarget=id;editingMessageId=null;ccClearReply();lastFeed='';document.title=originalPageTitle;
  el('draft').value=drafts.get(currentDraftKey())||'';
  el('sendError').textContent='';render();ccDrawOutbox();acknowledgeCurrentGroup();el('draft').focus();
}
function renderGroups() {
  const groups=el('groupList'),invites=el('groupInvites');
  if(!groups||!invites)return;
  groups.replaceChildren();invites.replaceChildren();
  for(const group of snapshot.groups||[]) {
    const button=document.createElement('button');button.type='button';button.className='cc-group-link';
    button.classList.toggle('active',groupTarget===group.id);
    const mark=document.createElement('span');mark.className='cc-group-avatar';
    if(group.photoData){const img=document.createElement('img');img.className='cc-group-avatar-image';img.alt='';img.src=group.photoData;mark.append(img);}
    else mark.textContent='👥';
    const label=document.createElement('span');label.className='cc-group-label';label.textContent=group.name;
    const count=groupUnreadCount(group.id);
    button.append(mark,label);
    if(group.muted){const mute=document.createElement('span');mute.className='cc-group-sidebar-muted';mute.textContent='🔕';mute.title='Muted group';button.append(mute);}
    if(count){const badge=document.createElement('span');badge.className='unread';badge.textContent=String(Math.min(99,count));button.append(badge);}
    button.onclick=()=>selectGroup(group.id);
    groups.append(button);
  }
  if(!(snapshot.groups||[]).length){const hint=document.createElement('p');hint.className='request-hint';hint.textContent='No groups yet. Create one or accept an invitation.';groups.append(hint);}
  const pending=snapshot.groupInvites||[];
  el('groupInviteCount').textContent=pending.length?String(pending.length):'';
  el('groupInviteCount').hidden=!pending.length;
  for(const invitation of pending){
    const row=document.createElement('div');row.className='cc-group-invitation';
    const text=document.createElement('p');text.textContent=invitation.inviter+' invited you to '+invitation.name;
    const buttons=document.createElement('div');buttons.className='request-actions';
    for(const [label,action] of [['Join ✓','accept'],['Decline','decline']]) {
      const b=document.createElement('button');b.type='button';b.textContent=label;
      if(action==='accept')b.className='request-yes';
      b.onclick=async()=>{
        [...buttons.children].forEach(child=>child.disabled=true);
        try{await api('groups/respond',{invite_id:String(invitation.inviteId),decision:action});
          await refresh();if(action==='accept')selectGroup(invitation.groupId);
          toast(action==='accept'?'Joined group!':'Group invitation declined.');
        }catch(e){toast(e.message);[...buttons.children].forEach(child=>child.disabled=false);}
      };
      buttons.append(b);
    }
    row.append(text,buttons);invites.append(row);
  }
}
function availableGroupInviteFriends(group) {
  const joined=new Set((group?.members||[]).map(m=>m.id));
  return (snapshot.contacts||[]).filter(person=>!joined.has(person.id));
}
function fillGroupFriendChoices(list,group=null) {
  list.replaceChildren();
  const friends=group ? availableGroupInviteFriends(group) : (snapshot.contacts||[]);
  if(!friends.length){const p=document.createElement('p');p.textContent='No eligible connected friends yet.';list.append(p);return;}
  for(const friend of friends){
    const label=document.createElement('label');label.className='cc-group-friend-choice';
    const input=document.createElement('input');input.type='checkbox';input.value=friend.publicId;
    const span=document.createElement('span');span.textContent=friend.displayName||friend.name;
    label.append(input,span);list.append(label);
  }
}
el('groupCreateOpen').onclick=()=>{
  el('groupCreateFeedback').textContent='';el('groupName').value='';
  fillGroupFriendChoices(el('groupCreateFriends'));
  el('groupCreateDialog').showModal();
};
el('groupCreateClose').onclick=()=>el('groupCreateDialog').close();
el('groupCreateForm').onsubmit=async event=>{
  event.preventDefault();const save=el('groupCreateSave');save.disabled=true;
  el('groupCreateFeedback').textContent='';
  try {
    const title=el('groupName').value.trim();
    const selected=[...el('groupCreateFriends').querySelectorAll('input:checked')].map(input=>input.value);
    const created=await api('groups/create',{name:title});
    let invited=0;const failures=[];
    for(const id of selected){try{await api('groups/invite',{group_id:created.groupId,public_id:id});++invited;}
      catch(err){failures.push(err.message);}}
    el('groupCreateDialog').close();await refresh();selectGroup(created.groupId);
    toast('Group created · '+invited+' invitation(s) sent'+(failures.length?' · '+failures.length+' failed':''));
  }catch(error){el('groupCreateFeedback').textContent=error.message;}
  finally{save.disabled=false;}
};
function currentGroup() {return (snapshot.groups||[]).find(group=>group.id===groupTarget);}
function updateGroupManage() {
  const group=currentGroup();if(!group){el('groupManageDialog').close();return;}
  const owner=group.ownerId===session.id;
  el('groupManageName').textContent=group.name;
  el('groupProfileTitle').textContent=group.name;
  el('groupDescriptionView').textContent=group.description || 'No description yet.';
  const photo=el('groupPhotoPreview'),placeholder=el('groupPhotoPlaceholder');
  photo.hidden=!group.photoData;
  placeholder.hidden=!!group.photoData;
  if(group.photoData && photo.src!==group.photoData)photo.src=group.photoData;
  if(!group.photoData)photo.removeAttribute('src');
  el('groupMuteToggle').textContent=group.muted?'🔔 Unmute group':'🔕 Mute group alerts';
  el('groupMuteToggle').setAttribute('aria-pressed',String(!!group.muted));
  el('groupDescriptionForm').hidden=!owner;
  el('groupPhotoForm').hidden=!owner;
  if(document.activeElement!==el('groupDescriptionInput'))el('groupDescriptionInput').value=group.description||'';
  el('groupPhotoRemove').disabled=!group.photoData;
  el('groupRenameForm').hidden=!owner;
  if(document.activeElement!==el('groupRenameName'))el('groupRenameName').value=group.name;
  el('groupOwnerName').textContent='Owner: '+group.ownerName;
  const members=el('groupMembers');members.replaceChildren();
  for(const member of group.members){
    const row=document.createElement('div');row.className='cc-member-row';
    const label=document.createElement('span');label.textContent=(member.displayName||member.name)+(member.id===group.ownerId?' 👑':'');
    row.append(label);
    if(owner && member.id!==session.id){
      const remove=document.createElement('button');remove.type='button';remove.textContent='Remove';
      remove.onclick=async()=>{
        if(!confirm('Remove '+member.name+' from '+group.name+'?'))return;
        remove.disabled=true;try{await api('groups/remove',{group_id:group.id,member_id:member.id});await refresh();updateGroupManage();}
        catch(e){el('groupManageFeedback').textContent=e.message;remove.disabled=false;}
      };row.append(remove);
    }
    members.append(row);
  }
  el('groupInviteForm').hidden=!owner;
  el('groupOwnerDelete').hidden=!owner;
  el('groupMemberLeave').hidden=owner;
  fillGroupFriendChoices(el('groupManageFriends'),group);
}
el('groupManageOpen').onclick=()=>{if(!currentGroup())return;
  el('groupManageFeedback').textContent='';updateGroupManage();el('groupManageDialog').showModal();};
el('groupRenameForm').onsubmit=async event=>{
  event.preventDefault();const group=currentGroup();if(!group || group.ownerId!==session.id)return;
  const name=el('groupRenameName').value.trim();const button=el('groupRenameSubmit');
  if(name.length<3 || name.length>64){el('groupManageFeedback').textContent='Group name must be 3–64 characters.';return;}
  button.disabled=true;el('groupManageFeedback').textContent='';
  try{await api('groups/rename',{group_id:group.id,name});await refresh();
      el('groupManageFeedback').textContent='Group name updated for all members.';toast('Group renamed!');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
  finally{button.disabled=false;}
};
el('groupMuteToggle').onclick=async()=>{
  const group=currentGroup();if(!group)return;
  const button=el('groupMuteToggle');button.disabled=true;el('groupManageFeedback').textContent='';
  const toMute=!group.muted;
  try{await api('groups/mute',{group_id:group.id,muted:toMute?'1':'0'});
      await refresh();updateGroupManage();toast(toMute?'Group alerts muted.':'Group alerts restored.');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
  finally{button.disabled=false;}
};
el('groupDescriptionForm').onsubmit=async event=>{
  event.preventDefault();const group=currentGroup();if(!group||group.ownerId!==session.id)return;
  const button=el('groupDescriptionSave');button.disabled=true;el('groupManageFeedback').textContent='';
  try{await api('groups/description',{group_id:group.id,description:el('groupDescriptionInput').value.trim()});
      await refresh();updateGroupManage();toast('Group description saved.');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
  finally{button.disabled=false;}
};
// Convert local PNG/JPEG/WebP to a small JPEG. Files never become publicly hosted URLs.
function makeSmallGroupPhoto(file){
  return new Promise((resolve,reject)=>{
    if(!file||file.size>5*1024*1024 || !/^image\/(jpeg|png|webp)$/.test(file.type)){
      reject(new Error('Choose a JPEG, PNG or WebP image under 5 MB.'));return;}
    const url=URL.createObjectURL(file),img=new Image();
    img.onload=()=>{
      try {
        const canvas=document.createElement('canvas');canvas.width=192;canvas.height=192;
        const ctx=canvas.getContext('2d');if(!ctx)throw new Error('Cannot process image.');
        const side=Math.min(img.naturalWidth,img.naturalHeight),x=(img.naturalWidth-side)/2,y=(img.naturalHeight-side)/2;
        ctx.drawImage(img,x,y,side,side,0,0,192,192);
        const data=canvas.toDataURL('image/jpeg',0.72);
        if(data.length>62000)throw new Error('Image still too large; choose a simpler photo.');
        resolve(data);
      }catch(e){reject(e);}finally{URL.revokeObjectURL(url);}
    };
    img.onerror=()=>{URL.revokeObjectURL(url);reject(new Error('Could not read this image.'));};
    img.src=url;
  });
}
el('groupPhotoForm').onsubmit=async event=>{
  event.preventDefault();const group=currentGroup();if(!group||group.ownerId!==session.id)return;
  const file=el('groupPhotoFile').files[0];if(!file){el('groupManageFeedback').textContent='Choose an image first.';return;}
  const button=el('groupPhotoSave');button.disabled=true;el('groupManageFeedback').textContent='';
  try{const photo_data=await makeSmallGroupPhoto(file);
      await api('groups/photo',{group_id:group.id,photo_data});
      el('groupPhotoFile').value='';await refresh();updateGroupManage();toast('Group photo updated.');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
  finally{button.disabled=false;}
};
el('groupPhotoRemove').onclick=async()=>{
  const group=currentGroup();if(!group||group.ownerId!==session.id)return;
  const button=el('groupPhotoRemove');button.disabled=true;el('groupManageFeedback').textContent='';
  try{await api('groups/photo',{group_id:group.id,photo_data:''});
      el('groupPhotoFile').value='';await refresh();updateGroupManage();toast('Group photo removed.');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
  finally{button.disabled=false;}
};
el('groupManageClose').onclick=()=>el('groupManageDialog').close();
el('groupInviteForm').onsubmit=async event=>{
  event.preventDefault();const group=currentGroup();if(!group)return;
  const selected=[...el('groupManageFriends').querySelectorAll('input:checked')].map(input=>input.value);
  if(!selected.length){el('groupManageFeedback').textContent='Select at least one friend.';return;}
  const button=el('groupSendInvites');button.disabled=true;let count=0;
  try {for(const id of selected){await api('groups/invite',{group_id:group.id,public_id:id});++count;}
    el('groupManageFeedback').textContent=count+' invitation(s) sent. Invitees must accept.';
    fillGroupFriendChoices(el('groupManageFriends'),group);
  }catch(e){el('groupManageFeedback').textContent=e.message;}
  finally{button.disabled=false;}
};
el('groupMemberLeave').onclick=async()=>{
  const group=currentGroup();if(!group||!confirm('Leave '+group.name+'? You will lose access to its conversation.'))return;
  try{await api('groups/leave',{group_id:group.id});el('groupManageDialog').close();
    groupTarget='';lastFeed='';await refresh();toast('Left group.');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
};
el('groupOwnerDelete').onclick=async()=>{
  const group=currentGroup();if(!group||!confirm('DELETE '+group.name+' for every member? Group history will be removed permanently.'))return;
  try{await api('groups/delete',{group_id:group.id});el('groupManageDialog').close();
    groupTarget='';lastFeed='';await refresh();toast('Group deleted for everyone.');}
  catch(e){el('groupManageFeedback').textContent=e.message;}
};

function select(id) {
  if(target!==id || groupTarget)window.ccVoiceCleanup?.();
  window.ccEmojiClose?.();
  if (id === target && !groupTarget) { acknowledgeCurrentChat(); return; }
  ccSearchPinned=null;ccSearchFocusId='';
  editingMessageId = null;
  drafts.set(currentDraftKey(), el('draft').value);
  ccStopTyping();
  target = id; groupTarget='';ccClearReply();
  document.title = originalPageTitle;
  el('draft').value = drafts.get(currentDraftKey()) || '';
  el('sendError').textContent = '';
  lastFeed = ''; render(); ccDrawOutbox(); el('draft').focus(); acknowledgeCurrentChat();
}
// Inline message editor. The server still verifies author_id against the session.
function editMessage(row, card, textNode) {
  if (!session || row.author !== session.id || row.deleted) return;
  editingMessageId = String(row.id);
  const form = document.createElement('form');
  form.className = 'message-edit-form';
  const input = document.createElement('textarea');
  input.className = 'message-edit-input';
  input.setAttribute('aria-label', 'Edit your message');
  input.maxLength = 2000;
  input.rows = 3;
  input.value = row.text;
  const actions = document.createElement('div');
  actions.className = 'message-edit-buttons';
  const save = document.createElement('button');
  save.type = 'submit'; save.textContent = 'Save ✓';
  const cancel = document.createElement('button');
  cancel.type = 'button'; cancel.textContent = 'Cancel';
  const error = document.createElement('span');
  error.className = 'message-action-error'; error.setAttribute('role','alert');
  const close = () => { editingMessageId = null; lastFeed = ''; render(); };
  cancel.onclick = close;
  form.onsubmit = async event => {
    event.preventDefault();
    const value = input.value.trim();
    if (!value || value.length > 2000) { error.textContent = 'Enter 1–2000 characters.'; return; }
    save.disabled = true; cancel.disabled = true; error.textContent = '';
    try {
      await api('messages/edit', {id:String(row.id),text:value});
      editingMessageId = null; lastFeed = '';
      await refresh();
    } catch (problem) {
      error.textContent = problem.message;
    } finally {save.disabled = false;cancel.disabled = false;}
  };
  actions.append(save,cancel); form.append(input,actions,error);
  textNode.replaceWith(form); input.focus(); input.setSelectionRange(input.value.length,input.value.length);
}
async function deleteMessageForEveryone(row, menu) {
  if (!session || row.author !== session.id || row.deleted) return;
  const promptText = row.to
    ? 'Delete for Everyone? Your friend will see a deleted-message label instead of this text.'
    : 'Delete this note for Everyone? A deleted-note label will remain in your notes.';
  if (!window.confirm(promptText)) return;
  const buttons = menu.querySelectorAll('button');
  buttons.forEach(b => {b.disabled = true;});
  try {
    await api('messages/delete', {id:String(row.id)});
    editingMessageId = null; lastFeed = '';
    await refresh();
  } catch (problem) {
    el('sendError').textContent = problem.message;
    buttons.forEach(b => {b.disabled = false;});
  }
}
// "Delete for Me" never deletes the shared DB row. It hides it for the
// current signed-in account, including after logout and login on another device.
async function deleteMessageForMe(row, menu) {
  if (!session) return;
  if (!window.confirm('Delete this message for you only? Your friend will still see it.')) return;
  const buttons = menu.querySelectorAll('button');
  buttons.forEach(button => {button.disabled = true;});
  try {
    await api('messages/delete-me', {id:String(row.id)});
    editingMessageId = null; lastFeed = '';
    // Remove it from this view immediately, even before a fresh state returns.
    snapshot.messages = (snapshot.messages || []).filter(message => String(message.id) !== String(row.id));
    render();
    await refresh();
  } catch (problem) {
    el('sendError').textContent = problem.message;
    buttons.forEach(button => {button.disabled = false;});
  }
}
// Group message controls use separate APIs from private DMs.
function editGroupMessage(row, textNode) {
  if(!session || row.author!==session.id || row.deleted || !groupTarget)return;
  const group=groupTarget;editingMessageId='g:'+String(row.id);
  const form=document.createElement('form');form.className='message-edit-form';
  const input=document.createElement('textarea');input.className='message-edit-input';
  input.setAttribute('aria-label','Edit your group message');input.maxLength=2000;input.rows=3;input.value=row.text;
  const buttons=document.createElement('div');buttons.className='message-edit-buttons';
  const save=document.createElement('button');save.type='submit';save.textContent='Save ✓';
  const cancel=document.createElement('button');cancel.type='button';cancel.textContent='Cancel';
  const error=document.createElement('span');error.className='message-action-error';error.setAttribute('role','alert');
  cancel.onclick=()=>{editingMessageId=null;lastFeed='';render();};
  form.onsubmit=async event=>{event.preventDefault();const value=input.value.trim();
    if(!value || value.length>2000){error.textContent='Enter 1–2000 characters.';return;}
    save.disabled=true;cancel.disabled=true;
    try{await api('groups/edit',{group_id:group,id:String(row.id),text:value});
      editingMessageId=null;lastFeed='';await refresh();toast('Group message edited.');}
    catch(e){error.textContent=e.message;}
    finally{save.disabled=false;cancel.disabled=false;}
  };
  buttons.append(save,cancel);form.append(input,buttons,error);textNode.replaceWith(form);input.focus();
}
async function deleteGroupMessage(row,forEveryone,menu){
  if(!session || !groupTarget)return;
  const group=groupTarget;
  const warning=forEveryone?'Delete for Everyone? Members will see a deleted-message label.':'Delete for Me? Only your account will hide this message.';
  if(!window.confirm(warning))return;
  const buttons=[...menu.querySelectorAll('button')];buttons.forEach(b=>b.disabled=true);
  try{await api(forEveryone?'groups/delete-message':'groups/delete-me',{
      group_id:group,id:String(row.id)});
      editingMessageId=null;lastFeed='';
      if(!forEveryone)snapshot.groupMessages=(snapshot.groupMessages||[]).filter(m=>!(m.groupId===group && String(m.id)===String(row.id)));
      await refresh();render();toast(forEveryone?'Message deleted for group.':'Message hidden for you.');}
  catch(e){el('sendError').textContent=e.message;buttons.forEach(b=>b.disabled=false);}
}
function renderBlockedAccounts() {
  const container=el('blockedPeople'); container.replaceChildren();
  const blocked = snapshot.blocked || [];
  if (!blocked.length) {
    const hint=document.createElement('p'); hint.className='request-hint';
    hint.textContent='No blocked accounts'; container.append(hint); return;
  }
  for (const person of blocked) {
    const row=document.createElement('div'); row.className='blocked-person';
    const text=document.createElement('span'); text.textContent='@' + person.name;
    const button=document.createElement('button'); button.type='button'; button.textContent='Unblock';
    button.setAttribute('aria-label','Unblock @' + person.name);
    button.onclick=async () => {
      button.disabled=true;
      try {
        await api('friends/unblock',{public_id:person.publicId});
        toast('@' + person.name + ' unblocked. A new friend request is needed to reconnect.');
        await refresh();
      } catch (error) {toast(error.message);}
      finally {button.disabled=false;}
    };
    row.append(text,button);container.append(row);
  }
}
// Separate global desktop permission from the selected friend's mute preference.
// The global alerts switch still works; a muted chat gets its own clear badge.
function updateActiveChatMuteBadge() {
  const anchor = el('alertsToggle');
  let badge = el('activeChatMuted');
  if (!badge) {
    badge = document.createElement('span');
    badge.id = 'activeChatMuted';
    badge.setAttribute('role', 'status');
    badge.setAttribute('aria-live', 'polite');
    anchor.before(badge);
  }
  const friend = (snapshot.contacts || []).find(person => person.id === target);
  const muted = Boolean(friend && friend.muted);
  badge.hidden = !muted;
  badge.textContent = muted ? '🔕 This chat muted' : '';
  badge.title = muted ? 'This friend is muted. Desktop alerts for other chats may still be on.' : '';
}

function render() {
  queueMicrotask(()=>window.ccMediaGallerySync?.());
  // Only accepted friends are selectable, never arbitrary registered accounts.
  peerNames = new Map((snapshot.contacts || []).map(person => [person.id, person.displayName || person.name]));
  if (target && !peerNames.has(target)) {
    target = ''; ccSearchPinned=null;ccSearchFocusId='';lastFeed = '';
  }
  if(groupTarget && !(snapshot.groups||[]).some(group=>group.id===groupTarget)) {
    groupTarget='';ccSearchPinned=null;ccSearchFocusId='';lastFeed='';
  }
  const selectedGroup=(snapshot.groups||[]).find(group=>group.id===groupTarget);
  const activeFriend = (snapshot.contacts || []).find(person => person.id === target);
  ccApplyWallpaper();
  updateActiveChatMuteBadge();
  renderGroups();
  el('ccChatWallpaperOpen').hidden=!selectedGroup&&!activeFriend;
  el('groupManageOpen').hidden=!selectedGroup;
  el('groupMuteStatus').hidden=!(selectedGroup && selectedGroup.muted);
  el('groupManageOpen').textContent=selectedGroup ? '👥 '+selectedGroup.members.length+' members' : '👥 Group info';
  el('friendProfileOpen').hidden = !activeFriend;
  el('title').textContent = selectedGroup ? selectedGroup.name : activeFriend ? (activeFriend.displayName || activeFriend.name) : 'My private notes';
  el('channelType').textContent = selectedGroup ? 'INVITE-ONLY GROUP · MEMBERS ONLY' : target ? 'PRIVATE CONVERSATION' : 'ONLY YOU CAN SEE THESE NOTES';
  el('room').classList.toggle('active', !target && !groupTarget);
  renderBlockedAccounts();
  const loungeUnread = unreadCount('');
  el('roomCount').textContent = loungeUnread ? (loungeUnread > 99 ? '99+' : loungeUnread) : '';
  el('roomCount').hidden = !loungeUnread;
  const people = el('people'); people.replaceChildren();
  const query = el('searchPeople').value.trim().toLowerCase();
  for (const [id, name] of peerNames) {
    if (!name.toLowerCase().includes(query)) continue;
    const button = document.createElement('button');
    const online = snapshot.users.some(u => u.id === id);
    button.className = 'person';
    button.classList.toggle('active', id === target);
    const avatar = document.createElement('span');
    avatar.className = 'person-avatar'; avatar.textContent = name.charAt(0).toUpperCase();
    const label = document.createElement('span');
    label.className = 'person-label'; label.textContent = name;
    if ((snapshot.contacts||[]).some(contact=>contact.id===id && contact.muted)) {
      label.textContent += ' 🔕';
    }
    const dot = document.createElement('span');
    dot.className = 'presence' + (online ? ' online' : '');
    dot.title = online ? 'Online' : 'Offline';
    button.append(avatar, label, dot);
    const count = unreadCount(id);
    if (count) {
      const badge = document.createElement('span');
      badge.className = 'unread'; badge.textContent = count > 99 ? '99+' : String(count);
      button.append(badge);
    }
    button.onclick = () => select(id);
    people.append(button);
  }
  if (!people.childElementCount) {
    const hint = document.createElement('p');
    hint.textContent = query ? 'No matching friends.' : 'No friends connected yet. Share your ID and accept requests!';
    people.append(hint);
  }
  // Show ONLY the selected thread's live typing. Backend has already filtered
  // membership and blocks, but we scope it again to prevent cross-chat leaks.
  const activeTyping = (snapshot.typing || []).filter(x =>
    groupTarget ? x.groupId === groupTarget : !!target && !x.groupId && x.author === target);
  const typedNames = [...new Set(activeTyping.map(x => x.name).filter(Boolean))];
  const typingLabel = !typedNames.length ? '' : typedNames.length===1 ?
    typedNames[0]+' is typing' : typedNames.length===2 ?
    typedNames.join(' and ')+' are typing' :
    typedNames[0]+' and '+(typedNames.length-1)+' others are typing';
  el('typing').textContent = typingLabel;
  el('typing').classList.toggle('cc-typing-on',!!typingLabel);
  // Make the same status obvious at the top of the chat on mobile/desktop.
  el('channelType').classList.toggle('cc-header-typing',!!typingLabel);
  if(typingLabel)el('channelType').textContent='● '+typingLabel+'…';
  const rows = currentMessages();
  ccRenderReplyBar();
  const hash = (groupTarget?'group:'+groupTarget:'friend:'+target) + JSON.stringify(rows);
  renderRequests();
  if (editingMessageId !== null) return; // Do not erase what the user is typing.
  if (hash === lastFeed) return;
  lastFeed = hash;
  const feed = el('feed');
  const nearBottom = feed.scrollHeight - feed.scrollTop - feed.clientHeight < 70;
  feed.replaceChildren();
  if (!rows.length) {
    const p = document.createElement('p');
    p.className = 'empty';
    p.textContent = groupTarget ? 'Start the group conversation 👥' : target ? 'Say hello to ' + el('title').textContent + ' 👋' : 'Your private notes are visible only to you. 🔒';
    feed.append(p);
  }
  for (const row of rows) {
    const card = document.createElement('article');
    card.className = 'bubble' + (row.author === session.id ? ' mine' : '');
    const by = document.createElement('b');
    const p = document.createElement('p');
    const stamp = document.createElement('time');
    by.textContent = row.author === session.id ? 'You' : row.name;
    p.textContent = row.deleted ? '🗑 This message was deleted' : row.text;
    if (row.deleted) p.classList.add('message-deleted');
    stamp.textContent = new Date(row.time * 1000).toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'});
    if (row.edited && !row.deleted) {
      const edited = document.createElement('span');
      edited.className = 'message-edited'; edited.textContent = ' · edited';
      edited.title = 'This message was edited'; stamp.append(edited);
    }
    if (!groupTarget && row.author === session.id && row.to) {
      const receipt = document.createElement('span');
      const delivery = ['sent','delivered','seen'].includes(row.delivery) ? row.delivery : 'sent';
      receipt.className = 'message-receipt status-' + delivery;
      receipt.textContent = delivery === 'sent' ? ' ✓ Sent' : delivery === 'delivered' ? ' ✓✓ Delivered' : ' ✓✓ Seen';
      receipt.title = delivery === 'sent' ? 'Stored securely on ChatConnect' :
        delivery === 'delivered' ? 'Acknowledged by your friend’s signed-in browser' :
          'Your friend opened this conversation';
      receipt.setAttribute('aria-label', delivery);
      stamp.append(receipt);
    }
    card.append(by, p, stamp);
    card.dataset.messageId=String(row.id);
    if(String(row.id)===ccSearchFocusId) card.classList.add('cc-search-highlight');
    if(ccSearchPinned?.row===row){const flag=document.createElement('small');flag.className='cc-search-older';flag.textContent='🔎 Older message found in search';card.prepend(flag);}
    if(row.reply){
      const quote=document.createElement('button');quote.type='button';quote.className='cc-reply-quote';
      const quotedName=document.createElement('strong');quotedName.textContent='↩ '+row.reply.name;
      const quotedText=document.createElement('span');quotedText.textContent=row.reply.text;
      quote.append(quotedName,quotedText);
      quote.title='Go to original message';quote.onclick=()=>{
        const original=[...feed.querySelectorAll('.bubble')].find(node=>node.dataset.messageId===String(row.reply.id));
        if(original){original.scrollIntoView({behavior:'smooth',block:'center'});
          original.classList.add('cc-reply-flash');setTimeout(()=>original.classList.remove('cc-reply-flash'),1400);}
        else toast('Original message is outside recent chat history.');
      };
      card.insertBefore(quote,p);
    }
    if(!row.deleted && row.attachment) renderAttachment(card,row.attachment);
    ccAddReactions(card,row);
    // Groups support author editing, owner moderation, and per-user hiding.
    if(groupTarget) {
      const group=currentGroup();const isMine=row.author===session.id;
      const isOwner=group && group.ownerId===session.id;
      const menu=document.createElement('details');menu.className='message-menu';
      const toggle=document.createElement('summary');toggle.textContent='⋯';
      toggle.title='Group message options';toggle.setAttribute('aria-label','Group message options');
      const options=document.createElement('div');options.className='message-menu-options';
      if(!row.deleted){const reply=document.createElement('button');reply.type='button';
        reply.textContent='↩ Reply';reply.onclick=()=>{menu.open=false;ccBeginReply(row);};options.append(reply);}
      if(isMine && !row.deleted && !row.attachment){const edit=document.createElement('button');edit.type='button';
        edit.textContent='✏️ Edit';edit.onclick=()=>{menu.open=false;editGroupMessage(row,p);};options.append(edit);}
      if((isMine || isOwner) && !row.deleted){const everyone=document.createElement('button');everyone.type='button';
        everyone.className='message-delete';everyone.textContent=isOwner&&!isMine?'🛡️ Remove for Everyone':'🗑️ Delete for Everyone';
        everyone.onclick=()=>{menu.open=false;deleteGroupMessage(row,true,menu);};options.append(everyone);}
      const mine=document.createElement('button');mine.type='button';mine.className='message-delete-me';
      mine.textContent='👤 Delete for Me';mine.onclick=()=>{menu.open=false;deleteGroupMessage(row,false,menu);};
      options.append(mine);menu.append(toggle,options);card.append(menu);
    }
    // Every participant can Delete for Me; only the original author may
    // Edit or Delete for Everyone. Even tombstones may be hidden locally.
    if (!groupTarget) {
      const menu = document.createElement('details');
      menu.className = 'message-menu';
      const toggle = document.createElement('summary');
      toggle.textContent = '⋯';
      toggle.title = 'Message options';
      toggle.setAttribute('aria-label','Message options');
      const options = document.createElement('div'); options.className = 'message-menu-options';
      if(target && !row.deleted){const reply=document.createElement('button');reply.type='button';
        reply.textContent='↩ Reply';reply.onclick=()=>{menu.open=false;ccBeginReply(row);};options.append(reply);}
      if (row.author === session.id && !row.deleted && !row.attachment) {
        const edit = document.createElement('button'); edit.type = 'button';
        edit.textContent = '✏️ Edit';
        edit.onclick = () => { menu.open = false; editMessage(row, card, p); };
        const removeEveryone = document.createElement('button'); removeEveryone.type = 'button';
        removeEveryone.textContent = '🗑️ Delete for Everyone'; removeEveryone.className = 'message-delete';
        removeEveryone.onclick = () => { menu.open = false; deleteMessageForEveryone(row, menu); };
        options.append(edit,removeEveryone);
      } else if (row.author === session.id && !row.deleted && row.attachment) {
        const removeEveryone=document.createElement('button');removeEveryone.type='button';
        removeEveryone.textContent='🗑️ Delete for Everyone';removeEveryone.className='message-delete';
        removeEveryone.onclick=()=>{menu.open=false;deleteMessageForEveryone(row,menu);};
        options.append(removeEveryone);
      }
      const removeMe = document.createElement('button'); removeMe.type = 'button';
      removeMe.textContent = '👤 Delete for Me'; removeMe.className = 'message-delete-me';
      removeMe.onclick = () => { menu.open = false; deleteMessageForMe(row, menu); };
      options.append(removeMe); menu.append(toggle,options); card.append(menu);
    }
    feed.append(card);
  }
  if (nearBottom) feed.scrollTop = feed.scrollHeight;
}
async function refresh() {
  if (busy || !session) return;
  busy = true;
  const current = session;
  try {
    const data = await api('state');
    if (session !== current) return;
    detectNewMessages(data);
    snapshot = data;
    ccUpdateAppBadge();
    if (data.publicId) {
      session.publicId = data.publicId;
      el('myPublicId').textContent = data.publicId;
    }
    el('status').textContent = (snapshot.users.length - 1) + ' friends online · Connected';
    if (el('groupManageDialog').open && groupTarget) updateGroupManage();
    if (el('friendProfileDialog').open && safetyFriendId) {
      if (!safetyContact()) el('friendProfileDialog').close();
      else updateSafetyPanel();
    }
    el('status').classList.remove('disconnected');
    render(); acknowledgeDeliveries(); acknowledgeCurrentChat(); acknowledgeCurrentGroup();
  } catch (error) {
    if (current !== session) return;
    if (error.status === 401) {
      clearSession(); el('joinError').textContent = error.message;
    } else {
      el('status').textContent = 'Reconnecting…';
      el('status').classList.add('disconnected');
    }
  } finally { busy = false; }
}
let signupMode = false;
function setAuthMode(signup) {
  signupMode = signup;
  el('joinError').textContent = '';
  el('tabLogin').classList.toggle('active', !signup);
  el('tabSignup').classList.toggle('active', signup);
  el('tabLogin').setAttribute('aria-pressed', String(!signup));
  el('tabSignup').setAttribute('aria-pressed', String(signup));
  document.querySelectorAll('.signup-only').forEach(section => {
    section.hidden = !signup;
    section.querySelectorAll('input').forEach(input => {input.disabled = !signup; input.required = signup;});
  });
  document.querySelectorAll('.signin-only').forEach(section => {
    section.hidden = signup;
    section.querySelectorAll('input').forEach(input => {input.disabled = signup; input.required = !signup;});
  });
  el('password').autocomplete = signup ? 'new-password' : 'current-password';
  el('login-heading').textContent = signup ? 'Create Your Account ✨' : 'Welcome Back 👋';
  el('login-intro').textContent = signup ? 'Join ChatConnect and meet your people.' : 'Your people. Your conversations. Your account.';
  el('passwordHint').textContent = signup ? 'Use a unique password with at least 10 characters.' : 'Your private account password — no room key needed.';
  el('authSubmit').textContent = signup ? '➜  Create Account & Chat' : '➜  Sign In & Chat';
}
el('tabLogin').onclick = () => setAuthMode(false);
el('tabSignup').onclick = () => setAuthMode(true);
el('authForm').onsubmit = async event => {
  event.preventDefault();
  const button = el('authSubmit'); button.disabled = true;
  el('joinError').textContent = '';
  try {
    if (signupMode && el('confirmPassword').value !== el('password').value) {
      throw new Error('Passwords do not match.');
    }
    const values = signupMode
      ? {name:el('name').value.trim(), email:el('email').value.trim(), password:el('password').value}
      : {identity:el('identity').value.trim(), password:el('password').value};
    ccClosePhoto(); ccCloseSearch(); ccSearchPinned=null; ccSearchFocusId='';
    clearAttachmentCache();
    session = await api(signupMode ? 'signup' : 'login', values, null);
    sessionStorage.setItem('cc.session', JSON.stringify(session));
    el('password').value = ''; el('confirmPassword').value = '';
    target = ''; groupTarget=''; readGroups.clear(); knownGroupInvites.clear(); lastGroupMessages.clear(); readAcknowledged.clear(); pendingRead.clear(); readRetryAt.clear(); deliveredAcknowledged.clear(); pendingDelivered.clear(); peerNames.clear(); inboxInitialized = false; lastMessageByFriend.clear(); knownRequests.clear();
    display();
  } catch (error) { el('joinError').textContent = error.message; }
  finally { button.disabled = false; }
};
el('compose').onsubmit = event => {
  event.preventDefault();
  const draft=el('draft'), text=draft.value.trim(), recipient=target, group=groupTarget;
  if(!text)return;
  if(new TextEncoder().encode(text).length>2000){
    el('sendError').textContent='Message is too long (2,000 UTF-8 bytes).';return;
  }
  try {
    const reply=(ccReplyTo && ccReplyTo.key===(group?'group:'+group:'friend:'+recipient))?ccReplyTo.id:'';
    ccQueueText(text,recipient,group,reply);
    ccStopTyping();
    draft.value='';drafts.delete(group?'group:'+group:'friend:'+recipient);
    ccClearReply();el('sendError').textContent='';
  }catch(error){el('sendError').textContent=error.message;}
};
// Typing presence is transient, never persisted with the offline text outbox.
function ccTypingRoute(){
  if(groupTarget)return {group_id:groupTarget};
  if(target)return {to:target};
  return null; // Notes are private to one account.
}
function ccStopTyping(){
  clearTimeout(typingTimer);
  const route=ccTypingRoute();
  lastTypingRequest=0;
  if(session && route)api('typing',{...route,typing:'0'}).catch(()=>{});
}
el('draft').addEventListener('input', () => {
  if (!session) return;
  clearTimeout(typingTimer);
  const route=ccTypingRoute();
  if(!route)return;
  if(!el('draft').value.trim()){ccStopTyping();return;}
  const timestamp=Date.now();
  if(timestamp-lastTypingRequest>=1600){
    lastTypingRequest=timestamp;
    api('typing',{...route,typing:'1'}).catch(()=>{});
  }
  typingTimer=setTimeout(()=>ccStopTyping(),2700);
});
// Blur should stop a stale typing indicator, including on mobile app switches.
window.addEventListener('blur',()=>{
  if(session && el('draft').value.trim())ccStopTyping();
});
el('searchPeople').addEventListener('input', render);
// Server-side message search. Never download conversation histories to browser for search.
let ccSearchScope='current', ccSearchSerial=0, ccSearchTimer=null;
const ccSearchDialog=el('ccSearchDialog');
function ccCloseSearch(){
  clearTimeout(ccSearchTimer);++ccSearchSerial;
  if(ccSearchDialog && ccSearchDialog.open)ccSearchDialog.close();
}
function ccSearchContext(){
  if(ccSearchScope==='all')return {context:'all',thread_id:''};
  if(groupTarget)return {context:'group',thread_id:groupTarget};
  if(target)return {context:'friend',thread_id:target};
  return {context:'notes',thread_id:''};
}
function ccSearchSetScope(scope){
  ccSearchScope=scope;
  el('ccSearchCurrent').setAttribute('aria-pressed',String(scope==='current'));
  el('ccSearchAll').setAttribute('aria-pressed',String(scope==='all'));
  ccSearchQueue();
}
function ccSearchOpen(){
  if(!session)return;
  ccSearchScope='current';
  el('ccSearchInput').value='';
  el('ccSearchCurrent').setAttribute('aria-pressed','true');
  el('ccSearchAll').setAttribute('aria-pressed','false');
  el('ccSearchResults').replaceChildren();
  el('ccSearchStatus').textContent='Search this '+(groupTarget?'group':target?'friend chat':'private notes')+' or switch to all chats. 🔐';
  if(!ccSearchDialog.open)ccSearchDialog.showModal();
  el('ccSearchInput').focus();
}
function ccSearchQueue(){
  clearTimeout(ccSearchTimer);
  const seq=++ccSearchSerial;
  if(!ccSearchDialog.open)return;
  const raw=el('ccSearchInput').value.trim();
  const box=el('ccSearchResults');box.replaceChildren();
  const searchBytes=new TextEncoder().encode(raw).length;
  if(searchBytes<2){el('ccSearchStatus').textContent='Type at least 2 characters (or an emoji) to search.';return;}
  if(searchBytes>160){el('ccSearchStatus').textContent='Search text is too long (160 UTF-8 bytes max).';return;}
  el('ccSearchStatus').textContent='Searching your private messages…';
  ccSearchTimer=setTimeout(async()=>{
    const requestingSession=session;
    const context=ccSearchContext();
    try{
      const data=await api('search/messages',{query:raw,...context});
      if(seq!==ccSearchSerial || session!==requestingSession || !ccSearchDialog.open)return;
      ccSearchRender(data,raw);
    }catch(error){
      if(seq!==ccSearchSerial || session!==requestingSession || !ccSearchDialog.open)return;
      box.replaceChildren();el('ccSearchStatus').textContent='Search failed: '+error.message;
    }
  },300);
}
function ccSearchRender(data,query){
  const results=Array.isArray(data.results)?data.results:[];
  const container=el('ccSearchResults');container.replaceChildren();
  el('ccSearchStatus').textContent=results.length+' result'+(results.length===1?'':'s')+
    (data.truncated?' (newest 60 shown — refine your search)':'')+' 🔒';
  if(!results.length){
    const empty=document.createElement('p');empty.className='cc-search-empty';
    empty.textContent='No matching messages. Try another word, or search all chats.';
    container.append(empty);return;
  }
  for(const hit of results){
    const item=document.createElement('button');item.type='button';item.className='cc-search-result';
    const top=document.createElement('span');top.className='cc-search-result-top';
    const conversation=document.createElement('strong');
    conversation.textContent=(hit.kind==='group'?'👥 ':hit.kind==='notes'?'🔒 ':'💬 ')+(hit.threadName||'Conversation');
    const when=document.createElement('time');when.textContent=new Date(Number(hit.time)*1000).toLocaleString();
    top.append(conversation,when);
    const from=document.createElement('small');from.textContent=hit.authorName||'Member';
    const body=document.createElement('span');body.className='cc-search-result-text';
    const message=String(hit.text||'') || (hit.attachment?'📎 '+hit.attachment.name:'');
    // Use textContent exclusively: message bodies are untrusted user text.
    const short=message.length>240?message.slice(0,240)+'…':message;
    const idx=short.toLocaleLowerCase().indexOf(query.toLocaleLowerCase());
    if(idx>=0){
      body.append(document.createTextNode(short.slice(0,idx)));
      const mark=document.createElement('mark');mark.textContent=short.slice(idx,idx+query.length);
      body.append(mark,document.createTextNode(short.slice(idx+query.length)));
    } else body.textContent=short;
    item.append(top,from,body);
    item.onclick=()=>ccSearchOpenHit(hit);
    container.append(item);
  }
}
function ccSearchOpenHit(hit){
  if(!session)return;
  const kind=hit.kind,threadId=String(hit.threadId||'');
  // Authorization can change while the search dialog is open. Only navigate
  // to currently visible friends/groups, matching the server's membership policy.
  if(kind==='friend' && !peerNames.has(threadId)){
    el('ccSearchStatus').textContent='Friend is no longer available. Refresh and search again.';return;
  }
  if(kind==='group' && !(snapshot.groups||[]).some(g=>g.id===threadId)){
    el('ccSearchStatus').textContent='Group access changed. Refresh and search again.';return;
  }
  ccCloseSearch();
  if(kind==='group')selectGroup(threadId);
  else select(kind==='notes'?'':threadId);
  const selectedRow={id:String(hit.id),author:String(hit.author),name:String(hit.authorName||'Member'),
    groupId:kind==='group'?threadId:undefined,to:kind==='group'?'':String(hit.to||''),
    text:String(hit.text||''),time:Number(hit.time),edited:false,deleted:false,
    attachment:hit.attachment||null,reactions:[],reply:null,delivery:''};
  ccSearchPinned={kind,threadId,row:selectedRow};
  ccSearchFocusId=String(hit.id);lastFeed='';render();
  const highlighted=[...el('feed').querySelectorAll('.bubble')]
    .find(node=>node.dataset.messageId===String(hit.id));
  if(highlighted){highlighted.scrollIntoView({behavior:'smooth',block:'center'});}
}
el('ccSearchOpen').onclick=ccSearchOpen;
el('ccSearchClose').onclick=ccCloseSearch;
el('ccSearchCurrent').onclick=()=>ccSearchSetScope('current');
el('ccSearchAll').onclick=()=>ccSearchSetScope('all');
el('ccSearchInput').addEventListener('input',ccSearchQueue);
ccSearchDialog.addEventListener('close',()=>{clearTimeout(ccSearchTimer);++ccSearchSerial;});

el('room').onclick = () => select('');
el('leave').onclick = async () => {
  try {
    await api('logout', {});
    // Explicit sign-out removes untransmitted local text on shared devices.
    if(ccOutboxUser)try{sessionStorage.removeItem(ccOutboxKey(ccOutboxUser));}catch{}
    clearSession();
  }
  catch (error) { el('sendError').textContent = error.message; }
};
el('export').onclick = () => {
  if (!session) return;
  const rows = currentMessages();
  if (!rows.length) { el('sendError').textContent = 'There are no messages to export here.'; return; }
  el('sendError').textContent = '';
  const title = groupTarget ? ((snapshot.groups||[]).find(g=>g.id===groupTarget)?.name||'Group') : target ? peerNames.get(target) || 'Friend' : 'My Private Notes';
  const fileContent = ['ChatConnect · ' + title, 'Exported: ' + new Date().toLocaleString(), '',
    ...rows.map(m => '[' + new Date(m.time * 1000).toLocaleString() + '] ' + m.name + ': ' + (m.deleted ? '[message deleted]' : m.text + (m.edited ? ' [edited]' : ''))), ''].join('\n');
  const blob = new Blob([fileContent], {type:'text/plain;charset=utf-8'});
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = 'ChatConnect_' + title.replace(/[^A-Za-z0-9_-]/g,'_') + '.txt';
  document.body.append(anchor); anchor.click(); anchor.remove();
  URL.revokeObjectURL(url);
};
if (session) display();

// Reveal password only when the user requests it.
el('revealKey').addEventListener('click', () => {
  const keyField = el('password'), button = el('revealKey');
  const show = keyField.type === 'password';
  keyField.type = show ? 'text' : 'password';
  button.textContent = show ? 'Hide' : 'Show';
  button.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
  button.setAttribute('aria-pressed', String(show));
});


// ============================================================================
// ChatConnect PWA + Mobile Navigation V1 (progressive enhancement only)
// No chat/message/attachment/API response is persisted by the service worker.
// ============================================================================
(function ccInitPwaMobileV1(){
  const head = document.head;
  const manifest = document.createElement('link');
  manifest.rel = 'manifest'; manifest.href = '/manifest.webmanifest'; head.append(manifest);
  const appleIcon = document.createElement('link');
  appleIcon.rel='apple-touch-icon'; appleIcon.href='/pwa-icon-180.png'; head.append(appleIcon);
  const appleCapable=document.createElement('meta');
  appleCapable.name='apple-mobile-web-app-capable'; appleCapable.content='yes'; head.append(appleCapable);
  const appleStatus=document.createElement('meta');
  appleStatus.name='apple-mobile-web-app-status-bar-style'; appleStatus.content='default'; head.append(appleStatus);

  if ('serviceWorker' in navigator && window.isSecureContext) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('/sw.js', {scope:'/'})
        .catch(error => console.warn('ChatConnect PWA registration unavailable:',error));
    });
    navigator.serviceWorker.addEventListener('message', event => {
      const info=event.data;
      if (!info || info.type !== 'cc-pwa-open' || !session) return;
      if(info.kind==='friend' && (snapshot.contacts||[]).some(x=>x.id===info.id)) select(info.id);
      if(info.kind==='group' && (snapshot.groups||[]).some(x=>x.id===info.id)) selectGroup(info.id);
    });
  }

  const isStandalone=()=>window.matchMedia('(display-mode: standalone)').matches ||
    window.navigator.standalone === true;
  const installButtons=[];
  let installEvent=null;
  function setInstallVisibility(){for(const b of installButtons)b.hidden=isStandalone();}
  function installButton(label){
    const b=document.createElement('button');b.type='button';
    b.className='cc-pwa-install';b.textContent=label;
    b.title='Install ChatConnect on this device';
    b.onclick=async()=>{
      if(isStandalone()){toast('ChatConnect is already installed.');return;}
      if(installEvent){
        const prompt=installEvent;installEvent=null;
        try{await prompt.prompt();await prompt.userChoice;}catch(error){console.warn(error);}
        setInstallVisibility();return;
      }
      if(!window.isSecureContext){
        toast('📱 Install needs HTTPS on your phone; localhost works on your own PC.');
      }else if(/iPad|iPhone|iPod/i.test(navigator.userAgent)){
        toast('📱 Safari: Share → Add to Home Screen.');
      }else{
        toast('📱 Browser menu (⋮) → Install app / Add to Home Screen.');
      }
    };
    installButtons.push(b);return b;
  }
  const entryHeader=document.querySelector('#entry .entry-header');
  if(entryHeader)entryHeader.append(installButton('📲 Install app'));
  const sidebarBrand=document.querySelector('#chat > aside .brand');
  if(sidebarBrand)sidebarBrand.after(installButton('📲 Install ChatConnect'));
  setInstallVisibility();
  window.addEventListener('beforeinstallprompt',event=>{
    event.preventDefault();installEvent=event;setInstallVisibility();
  });
  window.addEventListener('appinstalled',()=>{
    installEvent=null;setInstallVisibility();toast('📱 ChatConnect installed!');
  });

  // Compact phone experience: conversations occupy the screen; users open Chats
  // drawer for friends, groups, search, profile, theme and settings.
  const chat=el('chat'), aside=chat?.querySelector('aside'), threadHeader=chat?.querySelector('.thread > header');
  if(!chat||!aside||!threadHeader)return;
  const backdrop=document.createElement('button');
  backdrop.id='ccMobileBackdrop';backdrop.className='cc-mobile-backdrop';
  backdrop.type='button';backdrop.hidden=true;backdrop.setAttribute('aria-label','Close chats menu');
  const opener=document.createElement('button');opener.id='ccMobileChatsOpen';
  opener.type='button';opener.textContent='☰ Chats';opener.setAttribute('aria-expanded','false');
  opener.setAttribute('aria-controls','ccMobileChatMenu');
  aside.id='ccMobileChatMenu';
  threadHeader.prepend(opener);chat.append(backdrop);
  function syncDrawer(open){
    if(window.matchMedia('(min-width:721px)').matches)open=false;
    chat.classList.toggle('cc-mobile-drawer-open',open);
    opener.setAttribute('aria-expanded',String(open));
    backdrop.hidden=!open;
    if(open){const first=aside.querySelector('button');first?.focus({preventScroll:true});}
    else if(document.activeElement && aside.contains(document.activeElement))opener.focus({preventScroll:true});
  }
  window.ccCloseMobileDrawer=()=>syncDrawer(false);
  opener.onclick=()=>syncDrawer(!chat.classList.contains('cc-mobile-drawer-open'));
  backdrop.onclick=()=>syncDrawer(false);
  document.addEventListener('keydown',event=>{
    if(event.key==='Escape' && chat.classList.contains('cc-mobile-drawer-open'))syncDrawer(false);
  });
  document.addEventListener('click',event=>{
    if(event.target.closest('#room, #people .person, #groupList .cc-group-link'))syncDrawer(false);
  });
  window.addEventListener('resize',()=>{
    if(window.matchMedia('(min-width:721px)').matches)syncDrawer(false);
  });
})();

// ChatConnect Calls V1: load independently from the existing messaging UI.
(function(){const s=document.createElement("script");s.src="/cc-calls.js";document.body.appendChild(s);})();

// CC_MEDIA_GALLERY_V1 - Private, paginated gallery of photos/documents sent by you.
// The server filters by the authenticated author AND selected conversation.
(function ccInstallMediaGallery(){
  'use strict';
  const host=document.getElementById('ccSearchOpen');
  if(!host || document.getElementById('ccMediaOpen'))return;
  const button=document.createElement('button');
  button.id='ccMediaOpen';button.type='button';button.className='cc-media-open';
  button.textContent='🖼️ Media';button.title='Photos and files you sent in this conversation';
  button.setAttribute('aria-label','View photos and files I sent');
  host.insertAdjacentElement('afterend',button);

  const dialog=document.createElement('dialog');
  dialog.id='ccMediaDialog';dialog.className='cc-media-dialog';
  dialog.setAttribute('aria-labelledby','ccMediaHeading');
  dialog.innerHTML=`<section class="cc-media-panel">
    <header class="cc-media-top"><div><small>🔒 PRIVATE MEDIA</small>
      <h2 id="ccMediaHeading">My sent media</h2><p id="ccMediaContext"></p></div>
      <button id="ccMediaClose" type="button" aria-label="Close media gallery">✕</button></header>
    <nav class="cc-media-tabs" aria-label="Media type">
      <button type="button" data-media-kind="all" aria-pressed="true">All</button>
      <button type="button" data-media-kind="photos" aria-pressed="false">📸 Photos</button>
      <button type="button" data-media-kind="files" aria-pressed="false">📁 Files</button>
    </nav>
    <p id="ccMediaStatus" role="status" aria-live="polite"></p>
    <div id="ccMediaItems" class="cc-media-grid" aria-label="Sent photos and files"></div>
    <button id="ccMediaMore" type="button" hidden>Show older media ↓</button>
  </section>`;
  document.body.appendChild(dialog);
  const $=id=>document.getElementById(id);
  const items=$('ccMediaItems'),status=$('ccMediaStatus'),more=$('ccMediaMore');
  const tabs=[...dialog.querySelectorAll('[data-media-kind]')];
  let requestSerial=0,activeKey='',kind='all',cursor='',hasMore=false,loading=false;
  let seen=new Set();
  const ctx=()=>({scope:groupTarget?'group':target?'friend':'notes',
      thread:groupTarget||target||'',account:session?.id||''});
  const keyOf=c=>[c.account,c.scope,c.thread].join('|');
  const humanSize=n=>Number(n)>=1048576?(Number(n)/1048576).toFixed(1)+' MB':Math.max(0,Math.round(Number(n)/1024))+' KB';
  function close(){
    ++requestSerial;activeKey='';cursor='';loading=false;seen.clear();
    items.replaceChildren();status.textContent='';more.hidden=true;
    if(dialog.open)dialog.close();
  }
  window.ccMediaGalleryClose=close;
  window.ccMediaGallerySync=()=>{
    if(!dialog.open)return;
    if(!session||keyOf(ctx())!==activeKey)close();
  };
  function addEntry(file){
    if(!file||!/^\d+$/.test(String(file.id))||seen.has(String(file.id)))return;
    seen.add(String(file.id));
    const image=String(file.mime||'').startsWith('image/');
    const article=document.createElement('article');article.className='cc-media-item';
    if(image){
      const preview=document.createElement('button');preview.type='button';
      preview.className='cc-media-preview';preview.title='Open '+String(file.name||'photo');
      const picture=document.createElement('img');picture.alt='Preview: '+String(file.name||'Photo');
      picture.loading='lazy';picture.dataset.fileId=String(file.id);
      picture.addEventListener('error',()=>{preview.classList.add('cc-media-preview-failed');});
      preview.append(picture);
      preview.onclick=()=>{
        close();ccOpenPhoto({id:file.id,name:file.name,mime:file.mime,size:file.size});
      };
      article.append(preview);
      // Existing observer loads authenticated, private object-URL thumbnails on demand.
      if(typeof mediaObserver!=='undefined'&&mediaObserver)mediaObserver.observe(picture);
      else photoBlobUrl(file.id).then(url=>{if(picture.isConnected)picture.src=url;}).catch(()=>{});
    } else {
      const docIcon=document.createElement('div');docIcon.className='cc-media-file-icon';
      docIcon.textContent='📄';docIcon.setAttribute('aria-hidden','true');article.append(docIcon);
    }
    const name=document.createElement('strong');name.className='cc-media-name';
    name.textContent=String(file.name||'Untitled file');name.title=name.textContent;
    const info=document.createElement('small');info.className='cc-media-info';
    const date=new Date(Number(file.time)*1000);
    info.textContent=humanSize(file.size)+(Number.isFinite(date.getTime())?' · '+date.toLocaleDateString():'');
    const download=document.createElement('button');download.type='button';
    download.className='cc-media-download';download.textContent='↓ Download';
    download.onclick=()=>{
      try{requestNativeAttachmentDownload(file.id);}
      catch(error){status.textContent=error.message||'Download failed.';}
    };
    article.append(name,info,download);items.append(article);
  }
  async function loadPage(){
    if(!session || loading || !dialog.open)return;
    const request=++requestSerial, selected=ctx(), key=keyOf(selected), requestedKind=kind;
    if(key!==activeKey)return;
    loading=true;more.hidden=true;status.textContent='Loading your sent '+(kind==='all'?'media':kind)+'…';
    try{
      const response=await api('media/mine',{scope:selected.scope,thread_id:selected.thread,
         kind:requestedKind,before:cursor});
      if(request!==requestSerial||!dialog.open||keyOf(ctx())!==key||kind!==requestedKind)return;
      const rows=Array.isArray(response.items)?response.items:[];
      for(const file of rows)addEntry(file);
      cursor=String(response.next_before||'');
      hasMore=Boolean(response.has_more)&&!!cursor;
      more.hidden=!hasMore;
      status.textContent=seen.size?`${seen.size} item(s) shown${hasMore?' · more available':''}`:
        'No sent photos or files in this conversation.';
    }catch(error){
      if(request!==requestSerial||!dialog.open)return;
      status.textContent=error.message||'Could not load private media.';
    }finally{
      if(request===requestSerial){loading=false;}
    }
  }
  function switchTab(next){
    if(kind===next && seen.size)return;
    kind=next;cursor='';hasMore=false;seen.clear();items.replaceChildren();
    ++requestSerial;loading=false;
    for(const tab of tabs)tab.setAttribute('aria-pressed',String(tab.dataset.mediaKind===kind));
    loadPage();
  }
  function open(){
    if(!session)return;
    const c=ctx();activeKey=keyOf(c);kind='all';cursor='';hasMore=false;seen.clear();
    items.replaceChildren();
    const title=c.scope==='group'?(snapshot.groups||[]).find(g=>g.id===c.thread)?.name||'Group':
      c.scope==='friend'?peerNames.get(c.thread)||'Friend':'My private notes';
    $('ccMediaContext').textContent=`${title} · Only files you sent`;
    tabs.forEach(t=>t.setAttribute('aria-pressed',String(t.dataset.mediaKind==='all')));
    dialog.showModal();loadPage();
  }
  button.onclick=open;
  $('ccMediaClose').onclick=close;
  dialog.addEventListener('cancel',event=>{event.preventDefault();close();});
  tabs.forEach(t=>{t.onclick=()=>switchTab(t.dataset.mediaKind);});
  more.onclick=()=>{if(!loading&&hasMore)loadPage();};
})();


// CC_CHAT_LAYOUT_POLISH_V1 — presentation only; existing handlers/IDs are preserved.
(function installChatLayoutPolish(){
  'use strict';
  if (window.__ccChatLayoutPolishV1 || document.getElementById('ccv3SidebarToggle')) return;
  const shell=document.getElementById('chat');
  const sidebar=shell?.querySelector(':scope > aside');
  const thread=shell?.querySelector(':scope > .thread');
  const heading=thread?.querySelector(':scope > header');
  const actions=heading?.querySelector(':scope > .head-actions');
  const identity=heading?.querySelector(':scope > div:first-child');
  if(!shell||!sidebar||!thread||!heading||!actions||!identity||!identity.querySelector('#title'))return;
  window.__ccChatLayoutPolishV1=true;
  heading.classList.add('cc-ux-header');
  actions.classList.add('cc-ux-actions');
  identity.classList.add('cc-ux-identity');

  // A permanent toggle INSIDE the chat header; it remains reachable when sidebar is hidden.
  const leading=document.createElement('div');leading.className='cc-ux-leading';
  const toggle=document.createElement('button');
  toggle.id='ccUxSidebarToggle';toggle.type='button';toggle.className='cc-ux-sidebar-toggle';
  toggle.setAttribute('aria-controls','ccUxSidebar');
  sidebar.id='ccUxSidebar';
  const calls=document.createElement('div');calls.className='cc-ux-calls';
  calls.setAttribute('role','group');calls.setAttribute('aria-label','Voice and video calls');
  const badgeRow=document.createElement('div');badgeRow.className='cc-ux-status-row';
  identity.append(badgeRow);
  heading.insertBefore(leading,actions);
  leading.append(toggle,identity,calls);

  const main=document.createElement('div');main.className='cc-ux-main-actions';
  main.setAttribute('role','group');main.setAttribute('aria-label','Conversation tools');
  const extra=document.createElement('div');extra.className='cc-ux-extra-actions';
  extra.setAttribute('role','group');extra.setAttribute('aria-label','Chat preferences');
  actions.append(main,extra);

  let hidden=false;
  try{hidden=localStorage.getItem('cc-chat-sidebar-hidden-v1')==='1';}catch(_){}
  const update=()=>{
    shell.classList.toggle('cc-ux-sidebar-hidden',hidden);
    toggle.textContent=hidden?'☰':'☰';
    toggle.setAttribute('aria-expanded',String(!hidden));
    toggle.setAttribute('aria-label',hidden?'Show chats sidebar':'Hide chats sidebar');
    toggle.title=hidden?'Show chats sidebar':'Hide sidebar • Focus on conversation';
    toggle.setAttribute('data-mode',hidden?'show':'hide');
  };
  toggle.addEventListener('click',()=>{
    hidden=!hidden;update();
    try{localStorage.setItem('cc-chat-sidebar-hidden-v1',hidden?'1':'0');}catch(_){}
  });
  update();

  const selectGroup=el=>{
    const id=el.id||'';
    const label=((el.getAttribute('aria-label')||'')+' '+(el.title||'')+' '+(el.textContent||'')).toLowerCase();
    if(['status','groupMuteStatus','activeChatMuted'].includes(id)||el.tagName!=='BUTTON')return badgeRow;
    if(el.classList.contains('cc-call-launch')){
      return /history|call log|recent calls/.test(label)?main:calls;
    }
    if(['ccSearchOpen','ccMediaOpen','export'].includes(id))return main;
    if(/call history|call logs/.test(label))return main;
    if(['alertsToggle','friendProfileOpen','groupManageOpen'].includes(id))return extra;
    if(/wallpaper|profile|notification|desktop|mute|group info/.test(label))return extra;
    return extra;
  };
  const rank=(el,container)=>{
    if(container===main){
      if(el.id==='ccSearchOpen')return 10;
      if(el.id==='ccMediaOpen')return 20;
      const label=((el.title||'')+' '+(el.getAttribute('aria-label')||'')).toLowerCase();
      if(/call history|call log/.test(label))return 30;
      if(el.id==='export')return 40;
    }
    if(container===extra){
      const label=((el.title||'')+' '+(el.textContent||'')).toLowerCase();
      if(/wallpaper/.test(label))return 10;
      if(el.id==='friendProfileOpen')return 20;
      if(el.id==='groupManageOpen')return 30;
      if(el.id==='alertsToggle')return 40;
    }
    return 50;
  };
  function place(){
    // Move actual DOM nodes only, never clones: pre-existing click handlers remain attached.
    for(const element of [...actions.children]){
      if(element===main||element===extra)continue;
      selectGroup(element).appendChild(element);
    }
    for(const element of [...main.children,...extra.children]){
      const proper=selectGroup(element);
      if(proper!==element.parentNode)proper.appendChild(element);
    }
    for(const group of [main,extra]){
      // CSS visual order avoids unnecessary reparenting and preserves focus.
      for(const element of group.children)element.style.order=String(rank(element,group));
    }
    for(const element of calls.children){
      const label=((element.title||'')+' '+(element.getAttribute('aria-label')||'')).toLowerCase();
      element.style.order=/voice/.test(label)?'10':/video/.test(label)?'20':'30';
    }
  }
  place();
  // Call buttons/history and theme tools are created asynchronously by existing modules.
  // Only observe direct children of the original actions container, not nested groups.
  let queued=false;
  const observer=new MutationObserver(()=>{
    if(queued)return;
    queued=true;
    queueMicrotask(()=>{queued=false;place();});
  });
  observer.observe(actions,{childList:true});
  observer.observe(extra,{childList:true});
})();


// CC_REFINED_HEADER_FOCUS_V4 — five right-side tiles, sidebar controls beside voice/video.
(function ccRefinedHeaderFocusV4(){
  'use strict';
  const shell=document.getElementById('chat');
  const head=shell?.querySelector('.thread > header.ccv3-head');
  const aside=shell?.querySelector(':scope > aside');
  const actions=head?.querySelector(':scope > .head-actions');
  const toggle=document.getElementById('ccv3SidebarToggle');
  const calls=document.getElementById('ccv3Calls');
  const statusGroup=document.getElementById('ccv3Status');
  const secondary=document.getElementById('ccv3Secondary');
  if(!shell||!head||!aside||!actions||!toggle||!calls||!statusGroup||!secondary){
    console.warn('ChatConnect V4: expected V3 structure unavailable.');return;
  }
  if(window.__ccRefinedHeaderV4)return;
  window.__ccRefinedHeaderV4=true;

  const primary=document.createElement('div');
  primary.className='ccv3-square-tools ccv4-square-tools';
  primary.setAttribute('role','group');
  primary.setAttribute('aria-label','Conversation actions and connection status');
  actions.append(primary);

  const statusTile=document.createElement('div');
  statusTile.className='ccv4-status-tile';
  statusTile.title='Live friends connection status';
  statusTile.innerHTML='<span class="ccv4-status-icon" aria-hidden="true">👥</span><span class="ccv4-status-label">Connecting…</span>';
  primary.append(statusTile);
  const statusLabel=statusTile.querySelector('.ccv4-status-label');

  const chatsTile=document.createElement('button');
  chatsTile.type='button';
  chatsTile.className='ccv4-chats-tile';
  chatsTile.innerHTML='<span class="ccv3-tool-icon" aria-hidden="true">💬</span><span class="ccv3-tool-label">Chats</span>';
  chatsTile.setAttribute('aria-controls','ccv3Sidebar');
  primary.append(chatsTile);

  // Reuse the existing working Focus button; place it just LEFT of original calling buttons.
  calls.prepend(toggle);
  let hidden=false;
  try{hidden=localStorage.getItem('ccv3-focus-mode')==='1';}catch(_){}
  function focusMode(next){
    hidden=!!next;
    shell.classList.toggle('ccv3-focus',hidden);
    toggle.innerHTML=hidden?'<span aria-hidden="true">☰</span><small>Chats</small>':'<span aria-hidden="true">☰</span><small>Focus</small>';
    toggle.setAttribute('aria-expanded',String(!hidden));
    toggle.setAttribute('aria-label',hidden?'Show chats sidebar':'Hide chats sidebar');
    toggle.title=hidden?'Show chats sidebar':'Hide sidebar to expand conversation';
    chatsTile.setAttribute('aria-expanded',String(!hidden));
    chatsTile.setAttribute('aria-label',hidden?'Show chats sidebar':'Hide chats sidebar');
    chatsTile.title=hidden?'Show chats sidebar':'Hide chats sidebar';
  }
  toggle.addEventListener('click',()=>{
    focusMode(!hidden);
    try{localStorage.setItem('ccv3-focus-mode',hidden?'1':'0');}catch(_){}
  });
  chatsTile.addEventListener('click',()=>toggle.click());
  shell.classList.remove('cc-ux-sidebar-hidden','ccv2-focus');
  focusMode(hidden);

  const label=el=>((el.id||'')+' '+(el.getAttribute('aria-label')||'')+' '+(el.title||'')+' '+(el.textContent||'')).toLowerCase();
  function kind(el){
    if(el===statusTile||el===chatsTile||el===primary)return 'fixed';
    if(el===toggle)return 'focus';
    const id=el.id||'',l=label(el);
    if(id==='status')return 'liveStatus';
    if(id==='groupMuteStatus'||id==='activeChatMuted'||el.getAttribute('role')==='status')return 'metaStatus';
    if(id==='ccSearchOpen')return 'search';
    if(id==='ccMediaOpen')return 'media';
    if(id==='export')return 'export';
    if(/call history|call logs|recent calls/.test(l))return 'logs';
    if(el.classList.contains('cc-call-launch'))return 'calls';
    return 'secondary';
  }
  function decorate(el,type){
    if(!['search','media','logs','export'].includes(type))return;
    el.dataset.ccv3Tool=type;
    if(el.dataset.ccv4Decorated===type)return;
    const map={search:['🔎','Search'],media:['🖼️','Media'],logs:['📞','Calls'],export:['📤','Export']};
    const [symbol,name]=map[type];
    const icon=document.createElement('span');icon.className='ccv3-tool-icon';icon.setAttribute('aria-hidden','true');icon.textContent=symbol;
    const title=document.createElement('span');title.className='ccv3-tool-label';title.textContent=name;
    el.replaceChildren(icon,title); // keep same button node, listeners stay attached
    if(!el.title)el.title=name;
    if(type!=='logs')el.setAttribute('aria-label',type==='search'?'Search messages':name);
    el.dataset.ccv4Decorated=type;
  }
  function updateLiveStatus(){
    const source=document.getElementById('status');
    if(!source){statusLabel.textContent='Status unavailable';return;}
    const text=(source.textContent||'').trim();
    const count=text.match(/(\d+)\s+friends?/i);
    if(count&&/connected/i.test(text)){
      const n=Number(count[1]);statusLabel.textContent=n+' friend'+(n===1?'':'s')+' connected';
    }else if(count){
      const n=Number(count[1]);statusLabel.textContent=n+' friend'+(n===1?'':'s')+' online';
    }else{statusLabel.textContent=text||'Connecting…';}
    statusTile.setAttribute('aria-label',statusLabel.textContent);
  }
  const hosts={focus:calls,liveStatus:statusTile,metaStatus:statusGroup,search:primary,media:primary,logs:primary,export:primary,calls,secondary};
  let updating=false;
  function organize(){
    if(updating)return;
    updating=true;
    try{
      const all=[...actions.children,...primary.children,...calls.children,...statusTile.children,...statusGroup.children,...secondary.children].filter(el=>el!==primary&&el!==statusLabel&&el!==statusTile.querySelector('.ccv4-status-icon'));
      for(const el of all){
        const type=kind(el);
        if(type==='fixed'||!hosts[type])continue;
        const host=hosts[type];
        decorate(el,type);
        if(el.parentElement!==host){
          if(type==='focus')host.prepend(el);
          else host.append(el);
        }
        if(host===calls&&el!==toggle){el.style.order=/voice/.test(label(el))?'1':'2';}
      }
      toggle.style.order='0';
      updateLiveStatus();
      head.dataset.ccv4Ready='1';
    }finally{updating=false;}
  }
  organize();
  const source=document.getElementById('status');
  if(source){new MutationObserver(updateLiveStatus).observe(source,{childList:true,characterData:true,subtree:true});}
  let pending=false;
  const watcher=new MutationObserver(()=>{
    if(pending)return;
    pending=true;
    queueMicrotask(()=>{pending=false;organize();});
  });
  watcher.observe(actions,{childList:true});
  console.info('ChatConnect V4: five organized tiles, full-width Search, Focus alongside calls.');
})();


// CC_MOBILE_FOCUS_V5 — mobile-only, preserves existing ChatConnect controls and handlers.
(function ccMobileFocusV5(){
  'use strict';
  const shell=document.getElementById('chat');
  const head=shell?.querySelector('.thread > header.ccv3-head');
  const sidebar=shell?.querySelector(':scope > aside');
  const toggle=document.getElementById('ccv3SidebarToggle');
  const tools=head?.querySelector('.ccv4-square-tools');
  const statusSource=document.getElementById('status');
  const statusTile=tools?.querySelector('.ccv4-status-tile');
  if(!shell||!head||!sidebar||!toggle||!tools||!head.querySelector('#ccv3Secondary'))return;
  if(window.__ccMobileFocusV5)return;
  window.__ccMobileFocusV5=true;
  const mq=window.matchMedia('(max-width:720px)');
  let desktopFocusState=false;
  let mobileEntered=false;
  let moreOpened=false;
  const more=document.createElement('button');
  more.type='button'; more.className='ccv5-more-toggle';
  more.setAttribute('aria-controls','ccv3Secondary');
  more.setAttribute('aria-expanded','false');
  more.setAttribute('aria-label','Show wallpaper, profile and notification controls');
  more.textContent='⋯ More options';
  head.appendChild(more);
  function setMore(expanded){
    moreOpened=Boolean(expanded);
    head.classList.toggle('ccv5-more-open',moreOpened);
    more.setAttribute('aria-expanded',String(moreOpened));
    more.setAttribute('aria-label',moreOpened?'Hide extra conversation controls':'Show wallpaper, profile and notification controls');
    more.textContent=moreOpened?'⌃ Less options':'⋯ More options';
  }
  more.addEventListener('click',()=>setMore(!moreOpened));
  function updateMobileStatus(){
    if(!statusSource||!statusTile)return;
    const text=(statusSource.textContent||'').trim();
    const found=text.match(/(\d+)\s+friends?/i);
    statusTile.dataset.ccv5Short=found?`${found[1]} online`:(text.toLowerCase().includes('connected')?'Online':'Status');
  }
  updateMobileStatus();
  if(statusSource)new MutationObserver(updateMobileStatus).observe(statusSource,{childList:true,characterData:true,subtree:true});
  function hideDuplicateMobileChats(){
    // Old mobile navigation can insert a second full-width Chats bar. The V4 Chats tile replaces it.
    for(const el of shell.querySelectorAll('.thread button')){
      if(el===toggle||el.closest('.ccv4-square-tools')||el===more)continue;
      const label=(el.textContent||'').replace(/\s+/g,' ').trim();
      if(/^(?:☰|≡|☷)\s*Chats$/i.test(label))el.classList.add('ccv5-duplicate-chats');
    }
  }
  hideDuplicateMobileChats();
  let scanScheduled=false;
  new MutationObserver(()=>{
    if(scanScheduled)return;
    scanScheduled=true;
    queueMicrotask(()=>{scanScheduled=false;hideDuplicateMobileChats();});
  }).observe(shell.querySelector('.thread'),{childList:true,subtree:true});
  function syncResponsive(){
    if(mq.matches&&!mobileEntered){
      desktopFocusState=shell.classList.contains('ccv3-focus');
      mobileEntered=true;
      // Open directly on the conversation; both the Focus and Chats controls can reveal the sidebar.
      if(!shell.classList.contains('ccv3-focus'))toggle.click();
      setMore(false);
    } else if(!mq.matches&&mobileEntered){
      mobileEntered=false;
      if(shell.classList.contains('ccv3-focus')!==desktopFocusState)toggle.click();
      setMore(false);
    }
  }
  syncResponsive();
  if(mq.addEventListener)mq.addEventListener('change',syncResponsive);
  else if(mq.addListener)mq.addListener(syncResponsive);
  // On a phone, choosing a conversation from the sidebar returns to the focused chat view.
  sidebar.addEventListener('click',(event)=>{
    if(!mq.matches||shell.classList.contains('ccv3-focus'))return;
    const target=event.target.closest('#room, #people button, #groupList button');
    if(target)queueMicrotask(()=>{if(!shell.classList.contains('ccv3-focus'))toggle.click();});
  });
  console.info('ChatConnect V5 mobile: compact header, More options, chat-first layout.');
})();

// CC_MOBILE_CHATS_SCREEN_V7: replace broken V6 backdrop with a full-screen Chats list on phone.
(function ccMobileChatsScreenV7(){
  'use strict';
  if(window.__ccMobileChatsScreenV7) return;
  const shell=document.getElementById('chat');
  const sidebar=shell?.querySelector(':scope > aside');
  const toggle=document.getElementById('ccv3SidebarToggle');
  const chats=shell?.querySelector('.ccv4-chats-tile');
  if(!shell||!sidebar||!toggle||!chats){
    console.warn('ChatConnect V7: expected mobile elements missing.');return;
  }
  window.__ccMobileChatsScreenV7=true;
  const mq=window.matchMedia('(max-width:720px)');
  const bar=document.createElement('div');
  bar.className='ccv7-chats-header';
  const title=document.createElement('strong');
  title.textContent='Your chats';
  const back=document.createElement('button');
  back.type='button';
  back.className='ccv7-chats-back';
  back.textContent='← Back to chat';
  back.setAttribute('aria-label','Back to conversation');
  bar.append(title,back);
  const nav=sidebar.querySelector('nav');
  sidebar.insertBefore(bar,nav||sidebar.firstChild);
  let opened=false;
  let lastOpener=null;
  function setOpen(next,restoreFocus=false){
    opened=!!next&&mq.matches;
    shell.classList.toggle('ccv7-chats-open',opened);
    toggle.setAttribute('aria-expanded',String(opened));
    chats.setAttribute('aria-expanded',String(opened));
    const label=opened?'Close chats list':'Open chats list';
    toggle.setAttribute('aria-label',label);
    chats.setAttribute('aria-label',label);
    if(opened){back.focus({preventScroll:true});}
    else if(restoreFocus && lastOpener && document.contains(lastOpener)){
      lastOpener.focus({preventScroll:true});
    }
  }
  // Capture on document BEFORE V4 button listeners run; desktop clicks unaffected.
  document.addEventListener('click',event=>{
    if(!mq.matches)return;
    const control=event.target.closest('#ccv3SidebarToggle,.ccv4-chats-tile');
    if(!control||!shell.contains(control))return;
    event.preventDefault();
    event.stopImmediatePropagation();
    lastOpener=control;
    setOpen(!opened,true);
  },true);
  back.addEventListener('click',()=>setOpen(false,true));
  // Existing ChatConnect click handlers still handle actual conversation selection.
  sidebar.addEventListener('click',event=>{
    if(!opened||!mq.matches)return;
    const selected=event.target.closest('#room,#people .person,#people button,#groupList button,.cc-group-item,.cc-group-entry');
    if(selected)requestAnimationFrame(()=>setOpen(false,false));
  });
  document.addEventListener('keydown',event=>{
    if(opened&&mq.matches&&event.key==='Escape'){
      event.preventDefault();setOpen(false,true);
    }
  });
  function onViewport(){
    const mobile=mq.matches;
    shell.classList.toggle('ccv7-mobile-ready',mobile);
    if(mobile){
      // Mobile displays exactly one pane at a time; no backdrop required.
      shell.classList.add('ccv3-focus');
    }
    setOpen(false);
  }
  onViewport();
  if(mq.addEventListener)mq.addEventListener('change',onViewport);
  else if(mq.addListener)mq.addListener(onViewport);
  console.info('ChatConnect V7: mobile Chats page enabled (no dark backdrop).');
})();
