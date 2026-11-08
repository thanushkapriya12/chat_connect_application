/* ChatConnect Calls V1 — browser WebRTC media + authorized C++ signaling.
 * Calls require an open signed-in browser. STUN only is a local beta default;
 * install/configure TURN for reliable cross-network public deployments.
 * No recording, remote stream upload, tracking pixels or third-party SDKs.
 */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const header = document.querySelector('#chat .thread > header .head-actions');
  if (!header) return;
  const phone = document.createElement('button');
  const video = document.createElement('button');
  for (const [button,icon,label] of [[phone,'📞','Start voice call'],[video,'🎥','Start video call']]) {
    button.type='button';button.className='cc-call-launch';button.textContent=icon;
    button.title=label;button.setAttribute('aria-label',label);
    header.prepend(button);
  }
  // Incoming DM call history. Stored server-side and scoped to this signed-in user.
  const historyButton=document.createElement('button');
  historyButton.type='button';historyButton.className='cc-call-launch';
  historyButton.textContent='📋';historyButton.title='Incoming call history';
  historyButton.setAttribute('aria-label','Incoming call history');
  header.prepend(historyButton);
  const historyOverlay=document.createElement('div');
  historyOverlay.className='cc-call-history-overlay';historyOverlay.hidden=true;
  const historyCard=document.createElement('section');historyCard.className='cc-call-history-card';
  historyCard.setAttribute('role','dialog');historyCard.setAttribute('aria-label','Incoming call history');
  const historyTitle=document.createElement('h3');historyTitle.textContent='📞 Incoming call history';
  const historyClose=document.createElement('button');historyClose.type='button';
  historyClose.textContent='✕ Close';historyClose.onclick=()=>{historyOverlay.hidden=true;};
  const historyList=document.createElement('div');historyList.className='cc-call-history-list';
  historyCard.append(historyTitle,historyClose,historyList);historyOverlay.append(historyCard);
  document.body.append(historyOverlay);
  historyOverlay.addEventListener('click',e=>{if(e.target===historyOverlay)historyOverlay.hidden=true;});
  historyButton.onclick=async()=>{
    if(!session)return;
    historyOverlay.hidden=false;
    historyList.textContent='Loading call history…';
    try {
      const userId=session.id;
      const result=await api('calls/history');
      if(!session||session.id!==userId){historyOverlay.hidden=true;historyList.replaceChildren();return;}
      const calls=Array.isArray(result.calls)?result.calls:[];
      historyList.replaceChildren();
      if(!calls.length){historyList.textContent='No incoming calls yet.';return;}
      for(const item of calls){
        const row=document.createElement('div');row.className='cc-call-history-row';
        const title=document.createElement('strong');
        title.textContent=(item.status==='missed'?'🔴 ':item.status==='declined'?'⚪ ':'🟢 ')+String(item.from||'Friend');
        const detail=document.createElement('span');
        const time=new Date(Number(item.created)*1000);
        detail.textContent=String(item.mode||'voice')+' · '+String(item.status||'unknown')+' · '+(isNaN(time.getTime())?'':time.toLocaleString());
        row.append(title,detail);historyList.append(row);
      }
    }catch(e){historyList.textContent='Call history unavailable: '+(e?.message||'Unknown error');}
  };
  const overlay=document.createElement('div');overlay.className='cc-call-screen';overlay.hidden=true;
  overlay.innerHTML=`
    <section class="cc-call-card" role="dialog" aria-label="ChatConnect call" aria-modal="true">
      <div class="cc-call-top"><span>🔐 ChatConnect call</span><span id="ccCallClock"></span></div>
      <div class="cc-call-avatar">📞</div>
      <h2 id="ccCallWho">Calling...</h2><p id="ccCallStatus" role="status" aria-live="polite">Connecting</p>
      <div id="ccCallVideos" class="cc-call-grid"></div>
      <p id="ccCallWarning" class="cc-call-warning" hidden></p>
      <div id="ccCallIncoming" class="cc-call-actions" hidden>
        <button id="ccCallAccept" type="button" class="cc-call-accept">✅ Answer</button>
        <button id="ccCallDecline" type="button" class="cc-call-hangup">📵 Decline</button>
      </div>
      <div id="ccCallActive" class="cc-call-actions" hidden>
        <button id="ccCallMute" type="button">🎙️ Mute</button>
        <button id="ccCallVideo" type="button">📷 Camera off</button>
        <button id="ccCallSound" type="button">🔊 Enable sound</button>
        <button id="ccCallHangup" type="button" class="cc-call-hangup">📵 End call</button>
      </div>
    </section>`;
  document.body.append(overlay);
  let active=null, pending=null, local=null, iceConfig=null;
  let cursor=0, polling=false, lastIdentity='', since=0, closing=false;
  const peers=new Map();
  const soundTiles=new Map();
  const memberNames=new Map();
  let lastError='';
  let ringTimer=null,ringCtx=null;
  function stopRing(){if(ringTimer){clearInterval(ringTimer);ringTimer=null;}}
  function beep(){
    if(!pending||!session)return;
    try{
      const Audio=window.AudioContext||window.webkitAudioContext;
      if(!Audio)return;
      if(!ringCtx)ringCtx=new Audio();
      if(ringCtx.state==='suspended'){ringCtx.resume().catch(()=>{});return;}
      const start=ringCtx.currentTime;
      for(const [offset,frequency] of [[0,740],[0.23,920]]){
        const oscillator=ringCtx.createOscillator(),gain=ringCtx.createGain();
        oscillator.type='sine';oscillator.frequency.value=frequency;
        gain.gain.setValueAtTime(0,start+offset);
        gain.gain.linearRampToValueAtTime(0.035,start+offset+0.025);
        gain.gain.exponentialRampToValueAtTime(0.001,start+offset+0.17);
        oscillator.connect(gain);gain.connect(ringCtx.destination);
        oscillator.start(start+offset);oscillator.stop(start+offset+0.18);
      }
    }catch{}
  }
  function startRing(){stopRing();beep();ringTimer=setInterval(beep,3200);}
  document.addEventListener('pointerdown',()=>{
    if(pending&&ringCtx?.state==='suspended')ringCtx.resume().catch(()=>{});
  },{passive:true});
  const callApi = (route,body) => api('calls/'+route,body);
  const errorText = e => e?.message || String(e);
  function getSelected(){
    if(groupTarget)return {group_id:groupTarget};
    if(target)return {to:target};
    return null;
  }
  function showScreen(){overlay.hidden=false;}
  function showWarning(msg){$('ccCallWarning').textContent=msg;$('ccCallWarning').hidden=!msg;}
  function setStatus(text){$('ccCallStatus').textContent=text;}
  function showIncoming(room){
    pending=room;
    startRing();
    showScreen();showWarning('');
    $('ccCallIncoming').hidden=false;$('ccCallActive').hidden=true;
    const caller=room.members.find(p=>p.id===room.owner);
    $('ccCallWho').textContent=caller?.name||'ChatConnect friend';
    setStatus('🔔 Incoming '+room.mode+' call · '+(room.scope==='group'?'Group':'Private')+' · Accept or decline');
    $('ccCallClock').textContent='';
    document.title='📞 Incoming call · ChatConnect';
  }
  function updateView(){
    const ready=!!session&&!!getSelected()&&!active&&!pending;
    phone.hidden=video.hidden=!ready;
    phone.disabled=video.disabled=!ready;
    if(active){
      $('ccCallIncoming').hidden=true;$('ccCallActive').hidden=false;
      $('ccCallVideo').hidden=active.mode!=='video'||!local?.getVideoTracks()?.length;
      const tr=local?.getAudioTracks()?.[0];
      $('ccCallMute').textContent=tr?.enabled===false?'🔇 Unmute':'🎙️ Mute';
      const cam=local?.getVideoTracks()?.[0];
      $('ccCallVideo').textContent=cam?.enabled===false?'📷 Camera on':'📷 Camera off';
      $('ccCallClock').textContent=since?new Date((Date.now()-since)).toISOString().substring(11,19):'';
    }
  }
  function detachTiles(){
    $('ccCallVideos').replaceChildren();soundTiles.clear();memberNames.clear();
  }
  function cleanupMedia(){
    for(const entry of peers.values()){
      if(entry.dropTimer)clearTimeout(entry.dropTimer);
      try{entry.pc.ontrack=null;entry.pc.onicecandidate=null;entry.pc.close();}catch{}
    }
    peers.clear();
    if(local){for(const track of local.getTracks())track.stop();local=null;}
    detachTiles();
  }
  function clearCall(message=''){
    stopRing();
    cleanupMedia();pending=null;active=null;since=0;
    overlay.hidden=true;showWarning('');
    if(message&&session)toast(message);
    document.title=typeof originalPageTitle!=='undefined'?originalPageTitle:'ChatConnect';
    updateView();
  }
  async function endCall(){
    const room=active||pending;
    if(!room){clearCall();return;}
    closing=true;clearCall();
    try { if(session)await callApi('leave',{call_id:room.id}); }
    catch(e){console.warn('Call close will be cleared by server timeout:',e);}
    finally{closing=false;}
  }
  async function getMedia(mode, allowAudioOnlyFallback=false){
    if(!window.isSecureContext || !navigator.mediaDevices?.getUserMedia || !window.RTCPeerConnection)
      throw new Error('Calls require a recent browser on localhost or HTTPS, with mic/camera permission.');
    // Never ask for camera on voice-only calls.
    const constraints={audio:{echoCancellation:true,noiseSuppression:true,autoGainControl:true},
      video:mode==='video'?{width:{ideal:960},height:{ideal:540},frameRate:{ideal:24}}:false};
    try{return await navigator.mediaDevices.getUserMedia(constraints);}
    catch(e){
      // Receiver only: a busy webcam must not block microphone-only participation.
      // Do not bypass permission denials, and do not silently downgrade callers.
      if(mode!=='video'||!allowAudioOnlyFallback||!['NotReadableError','AbortError','TrackStartError'].includes(e?.name))throw e;
      return navigator.mediaDevices.getUserMedia({audio:constraints.audio,video:false});
    }
  }
  async function config(){
    if(!iceConfig)iceConfig=(await callApi('config')).iceServers||[];
    return iceConfig;
  }
  function appendLocal(){
    if(!local||(active?.mode==='video'&&!local.getVideoTracks().length))return;
    const box=document.createElement('div');box.className='cc-call-tile cc-call-local';
    const preview=document.createElement('video');
    preview.autoplay=true;preview.muted=true;preview.playsInline=true;preview.srcObject=local;
    box.append(preview);
    const label=document.createElement('span');label.textContent='You';box.append(label);
    $('ccCallVideos').prepend(box);
    preview.play().catch(()=>{});
  }
  function makeRemoteTile(id){
    if(soundTiles.has(id))return soundTiles.get(id);
    const box=document.createElement('div');box.className='cc-call-tile';
    const media=document.createElement(active.mode==='video'?'video':'audio');
    media.autoplay=true;media.playsInline=true;media.setAttribute('aria-label','Remote call media');
    box.append(media);
    const name=document.createElement('span');name.textContent=memberNames.get(id)||'Friend';box.append(name);
    $('ccCallVideos').append(box);
    const tile={box,media};soundTiles.set(id,tile);return tile;
  }
  async function sendSignal(to,kind,data,callId){
    if(!session||!active||active.id!==callId)return;
    try {await callApi('signal',{call_id:callId,to,kind,payload:JSON.stringify(data)});}
    catch(e){console.warn('Call signal failed:',kind,errorText(e));showWarning('Connection signaling interrupted. Check network and try again.');}
  }
  async function ensurePeer(id){
    if(peers.has(id))return peers.get(id);
    if(!active||!local)throw new Error('Call not ready for connection.');
    const roomId=active.id;
    const pc=new RTCPeerConnection({iceServers:await config()});
    const entry={pc,candidates:[],offered:false,dropTimer:null};peers.set(id,entry);
    for(const track of local.getTracks())pc.addTrack(track,local);
    // If this participant has no webcam, still negotiate an incoming video m-line.
    // Needed if the audio-only receiver becomes the WebRTC offerer.
    if(active.mode==='video'&&!local.getVideoTracks().length)pc.addTransceiver('video',{direction:'recvonly'});
    pc.ontrack=event=>{
      if(!active||active.id!==roomId)return;
      const tile=makeRemoteTile(id);
      tile.media.srcObject=event.streams[0]||new MediaStream([event.track]);
      tile.media.play().catch(()=>showWarning('Tap Enable sound to hear the other person.'));
    };
    pc.onicecandidate=event=>{
      if(event.candidate)void sendSignal(id,'ice',event.candidate.toJSON(),roomId);
    };
    pc.onconnectionstatechange=()=>{
      if(!active||active.id!==roomId)return;
      if(pc.connectionState==='connected'){
        if(entry.dropTimer){clearTimeout(entry.dropTimer);entry.dropTimer=null;}
        setStatus('🟢 Connected · '+(active.scope==='group'?'Group':'Private')+' call');
        showWarning(active.mode==='video'&&!local?.getVideoTracks()?.length
          ? 'Your camera is busy. Joined with microphone only; incoming video is still available.' : '');
      }
      else if(['disconnected','failed'].includes(pc.connectionState)){
        showWarning('Call connection interrupted. Waiting briefly for the network to recover…');
        // Only DM: allow transient Wi-Fi changes, then release devices on prolonged loss.
        // Group calls intentionally keep the room when just one participant drops.
        if(active.scope==='dm' && !entry.dropTimer){
          entry.dropTimer=setTimeout(()=>{
            entry.dropTimer=null;
            if(!active || active.id!==roomId || peers.get(id)!==entry)return;
            if(!['disconnected','failed'].includes(pc.connectionState))return;
            toast('Call connection lost. Please call again.');
            void endCall();
          },pc.connectionState==='failed'?12000:25000);
        }
      }
    };
    return entry;
  }
  async function flushIce(entry){
    if(!entry.pc.remoteDescription)return;
    for(const candidate of entry.candidates.splice(0)){
      try{await entry.pc.addIceCandidate(candidate);}catch(e){console.warn('ICE candidate rejected:',e);}
    }
  }
  async function processSignal(ev){
    if(!active||ev.call!==active.id||!local||!ev.from)return;
    try{
      const payload=JSON.parse(ev.payload);
      if(ev.kind==='offer'){
        const entry=await ensurePeer(ev.from);
        await entry.pc.setRemoteDescription(payload);
        await flushIce(entry);
        const answer=await entry.pc.createAnswer();
        await entry.pc.setLocalDescription(answer);
        await sendSignal(ev.from,'answer',entry.pc.localDescription,active.id);
      }else if(ev.kind==='answer'){
        const entry=await ensurePeer(ev.from);
        if(entry.pc.signalingState==='have-local-offer'){
          await entry.pc.setRemoteDescription(payload);await flushIce(entry);
        }
      }else if(ev.kind==='ice'){
        const entry=await ensurePeer(ev.from);
        if(entry.pc.remoteDescription)await entry.pc.addIceCandidate(payload);
        else entry.candidates.push(payload);
      }
    }catch(e){console.warn('Call WebRTC negotiation failed:',e);showWarning('Call negotiation error: '+errorText(e));}
  }
  async function reconcilePeers(room){
    if(!active||active.id!==room.id||!local)return;
    const accepted=room.members.filter(p=>p.id!==session.id&&p.status==='active');
    const list=new Set(accepted.map(p=>p.id));
    for(const [id,entry] of peers){
      if(!list.has(id)){
        if(entry.dropTimer)clearTimeout(entry.dropTimer);
        try{entry.pc.close();}catch{}
        peers.delete(id);
        soundTiles.get(id)?.box.remove();soundTiles.delete(id);
      }
    }
    for(const p of accepted){
      memberNames.set(p.id,p.name);
      // Exactly one side initiates each peer offer, avoiding glare.
      if(session.id.localeCompare(p.id)<0){
        const entry=await ensurePeer(p.id);
        if(!entry.offered){
          entry.offered=true;
          try{
            const offer=await entry.pc.createOffer();
            await entry.pc.setLocalDescription(offer);
            await sendSignal(p.id,'offer',entry.pc.localDescription,room.id);
          }catch(e){entry.offered=false;throw e;}
        }
      }
    }
    if(accepted.length && ![...peers.values()].some(p=>p.pc.connectionState==='connected'))
      setStatus('📡 Connecting audio/video…');
  }
  async function start(mode){
    if(!session||active||pending||closing)return;
    const route=getSelected();if(!route)return;
    phone.disabled=video.disabled=true;
    try{
      local=await getMedia(mode);
      const call=await callApi('start',{...route,mode});
      active=call;since=Date.now();
      showScreen();$('ccCallWho').textContent=call.scope==='group'?(snapshot.groups||[]).find(g=>g.id===call.thread)?.name||'Group':
        (snapshot.contacts||[]).find(p=>p.id===call.thread)?.displayName||
        (snapshot.contacts||[]).find(p=>p.id===call.thread)?.name||'Friend';
      $('ccCallIncoming').hidden=true;$('ccCallActive').hidden=false;
      setStatus('🔔 Ringing…');showWarning('');
      appendLocal();updateView();
    }catch(e){cleanupMedia();showWarning('');toast('📞 '+errorText(e));}
    finally{updateView();}
  }
  async function accept(){
    if(!pending||active||closing)return;
    const invite=pending;
    $('ccCallAccept').disabled=true;
    try{
      local=await getMedia(invite.mode,true);
      const reply=await callApi('decision',{call_id:invite.id,accept:'1'});
      if(!session){cleanupMedia();return;}
      active=reply;pending=null;stopRing();since=Date.now();
      showScreen();$('ccCallIncoming').hidden=true;$('ccCallActive').hidden=false;
      $('ccCallWho').textContent=invite.scope==='group'?'Group call':
        invite.members.find(p=>p.id!==session.id)?.name||'Friend';
      setStatus('📡 Connecting…');
      showWarning(invite.mode==='video'&&!local.getVideoTracks().length
        ? 'Your camera is busy. Joined with microphone only; incoming video is still available.' : '');
      appendLocal();updateView();
    }catch(e){cleanupMedia();showWarning(errorText(e));setStatus('Cannot answer call');}
    finally{$('ccCallAccept').disabled=false;}
  }
  async function decline(){
    const invite=pending;if(!invite)return;
    clearCall();
    try{await callApi('decision',{call_id:invite.id,accept:'0'});}catch(e){console.warn('Decline unavailable:',e);}
  }
  async function poll(){
    if(polling||closing)return;
    if(!session){
      if(active||pending)clearCall();
      lastIdentity='';cursor=0;return;
    }
    const identity=session.id;
    if(lastIdentity!==identity){lastIdentity=identity;cursor=0;clearCall();}
    polling=true;
    try{
      const data=await callApi('poll',{cursor:String(cursor)});
      if(!session||session.id!==identity)return;
      const events=Array.isArray(data.events)?data.events:[];
      cursor=Math.max(cursor,Number(data.cursor)||0);
      const room=data.call;
      if(!room){
        if(active||pending)clearCall('Call ended');
      }else if(room.myStatus==='invited'&&!active){
        if(!pending||pending.id!==room.id)showIncoming(room);
      }else if(room.myStatus==='active'){
        if(active&&room.id===active.id){
          active=room;
          for(const m of room.members)memberNames.set(m.id,m.name);
          for(const ev of events)if(['offer','answer','ice'].includes(ev.kind))await processSignal(ev);
          await reconcilePeers(room);
          if(room.members.length<=1 && Date.now()-since>65000){
            toast('Nobody answered this call.');await endCall();
          }
        }else if(!active && !pending){
          // Recovering after page reload requires fresh track/SDP negotiation.
          // End the orphaned call rather than appearing connected without media.
          await callApi('leave',{call_id:room.id}).catch(()=>{});
          showWarning('');toast('Previous call closed after reload; call again.');
        }
      }
      // Older events are ignored when we are not a current participant.
      if(active&&events.some(e=>e.call===active.id&&e.kind==='ended'))clearCall('Call ended');
    }catch(e){
      if((e?.status===401 || e?.status===403) && (active||pending))
        clearCall(e.status===401?'Session expired. Call ended':'Call permission changed');
      else if(active)showWarning('Network interrupted: '+errorText(e));
      else if(!lastError || lastError!==errorText(e))console.warn('Call polling unavailable:',e);
      lastError=errorText(e);
    }finally{polling=false;updateView();}
  }
  window.ccCallsCleanup=()=>{historyOverlay.hidden=true;historyList.replaceChildren();if(active||pending)void endCall();else cleanupMedia();};
  phone.onclick=()=>void start('voice');
  video.onclick=()=>void start('video');
  $('ccCallAccept').onclick=()=>void accept();
  $('ccCallDecline').onclick=()=>void decline();
  $('ccCallHangup').onclick=()=>void endCall();
  $('ccCallMute').onclick=()=>{
    const track=local?.getAudioTracks()?.[0];if(track)track.enabled=!track.enabled;updateView();
  };
  $('ccCallVideo').onclick=()=>{
    const track=local?.getVideoTracks()?.[0];if(track)track.enabled=!track.enabled;updateView();
  };
  $('ccCallSound').onclick=()=>{
    for(const {media} of soundTiles.values())media.play().catch(()=>{});
    showWarning('');
  };
  // Polls go through bearer authentication; never place secrets in URL.
  // Stop local tracks immediately on sign-out / tab exit.
  document.addEventListener('visibilitychange',()=>{if(!document.hidden)void poll();});
  window.addEventListener('pagehide',()=>{
    const room=active||pending;
    cleanupMedia();
    if(room&&session){
      fetch('/api/calls/leave',{method:'POST',keepalive:true,
        headers:{Authorization:'Bearer '+session.token},
        body:new URLSearchParams({call_id:room.id})}).catch(()=>{});
    }
  });
  setInterval(()=>{updateView();void poll();},1400);
  updateView();void poll();
})();
