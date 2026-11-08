/* ChatConnect offline emoji keyboard and authenticated voice notes.
 * Depends only on current ChatConnect app.js and locally bundled emoji data.
 * No CDN, microphone audio does not leave the browser until Send is clicked.
 */
(() => {
  'use strict';
  const E = id => document.getElementById(id);
  const VOICE_LIMIT = 300*1024*1024; // Same limit as the chunked attachment uploader.
  // Duration is determined by audio bitrate and file size; no fixed minute cap.
  const EMOJI_PAGE = 120;
  const allEmojis = Array.isArray(window.CC_EMOJI_DATA) ? window.CC_EMOJI_DATA : [];
  const categories = [
    ['😀','Faces'],['👋','People'],['🐼','Nature'],['🍓','Food'],
    ['🌍','Travel'],['🎮','Fun'],['💡','Objects'],['💞','Symbols'],['🏁','Flags']
  ];
  let emojiCategory = 0, emojiList = [], emojiShown = 0;
  let emojiSearchTimer = 0;
  let recentEmoji = [];
  let caretStart = null, caretEnd = null;
  const draft = E('draft'), emojiPanel = E('ccEmojiPanel');
  const emojiTrigger = E('ccEmojiButton');
  if (!draft || !emojiPanel || !emojiTrigger) return;
  try {const value=JSON.parse(localStorage.getItem('cc.emoji.recent')||'[]');
       if(Array.isArray(value))recentEmoji=value.filter(x=>typeof x==='string').slice(0,28);
  }catch{}
  function captureCaret(){caretStart=draft.selectionStart;caretEnd=draft.selectionEnd;}
  draft.addEventListener('select',captureCaret);
  draft.addEventListener('keyup',captureCaret);
  draft.addEventListener('click',captureCaret);
  function insertEmoji(glyph){
    const start=Number.isInteger(caretStart)?caretStart:draft.value.length;
    const end=Number.isInteger(caretEnd)?caretEnd:draft.value.length;
    if (draft.value.length-(end-start)+glyph.length>Number(draft.maxLength) ||
        new TextEncoder().encode(draft.value.slice(0,start)+glyph+draft.value.slice(end)).length>2000) {
      E('sendError').textContent='Message limit reached (2,000 characters).';return;
    }
    draft.setRangeText(glyph,start,end,'end');
    captureCaret();draft.dispatchEvent(new Event('input',{bubbles:true}));
    draft.focus({preventScroll:true});
    recentEmoji=[glyph,...recentEmoji.filter(x=>x!==glyph)].slice(0,28);
    try{localStorage.setItem('cc.emoji.recent',JSON.stringify(recentEmoji));}catch{}
  }
  function emojiNode(glyph,name){
    const button=document.createElement('button');button.type='button';
    button.textContent=glyph;button.title=name;button.setAttribute('aria-label',name);
    button.onclick=()=>insertEmoji(glyph);return button;
  }
  function selectEmojiCategory(next){
    emojiCategory=next;E('ccEmojiSearch').value='';updateEmojiList();
  }
  function renderEmojiTabs(){
    const nav=E('ccEmojiTabs');nav.replaceChildren();
    const items=[['🕘','Recent'],...categories];
    for(let i=0;i<items.length;++i){
      const id=i-1;
      const button=document.createElement('button');button.type='button';
      button.textContent=items[i][0];button.title=items[i][1];
      button.setAttribute('aria-label',items[i][1]);
      button.setAttribute('aria-selected',String(emojiCategory===id));
      button.onclick=()=>selectEmojiCategory(id);
      nav.append(button);
    }
  }
  function matchingEmoji(){
    const query=E('ccEmojiSearch').value.trim().toLowerCase();
    if(query){const terms=query.split(/\s+/).filter(Boolean);
      return allEmojis.filter(([glyph,name])=>terms.every(term=>name.includes(term)||glyph===term));
    }
    if(emojiCategory===-1){
      const lookup=new Map(allEmojis.map(([glyph,name])=>[glyph,name]));
      return recentEmoji.filter(glyph=>lookup.has(glyph)).map(glyph=>[glyph,lookup.get(glyph)]);
    }
    return allEmojis.filter(row=>row[2]===emojiCategory);
  }
  function showEmojiPage(reset=false){
    if(reset){E('ccEmojiGrid').replaceChildren();emojiShown=0;}
    const next=emojiList.slice(emojiShown,emojiShown+EMOJI_PAGE);
    const fragment=document.createDocumentFragment();
    for(const [glyph,name] of next)fragment.append(emojiNode(glyph,name));
    E('ccEmojiGrid').append(fragment);
    emojiShown+=next.length;
    E('ccEmojiMore').hidden=emojiShown>=emojiList.length;
    E('ccEmojiCount').textContent=emojiList.length?`${emojiShown} / ${emojiList.length} emojis`:'No matching emojis';
  }
  function updateEmojiList(){
    renderEmojiTabs();emojiList=matchingEmoji();showEmojiPage(true);
  }
  function closeEmoji(){emojiPanel.hidden=true;emojiTrigger.setAttribute('aria-expanded','false');}
  window.ccEmojiClose=closeEmoji;
  emojiTrigger.setAttribute('aria-expanded','false');
  emojiTrigger.onclick=()=>{
    if(!session){E('sendError').textContent='Sign in to use the emoji keyboard.';return;}
    const show=emojiPanel.hidden;
    emojiPanel.hidden=!show;emojiTrigger.setAttribute('aria-expanded',String(show));
    if(show){updateEmojiList();E('ccEmojiSearch').focus();}
  };
  E('ccEmojiClose').onclick=closeEmoji;
  E('ccEmojiMore').onclick=()=>showEmojiPage(false);
  E('ccEmojiSearch').oninput=()=>{
    clearTimeout(emojiSearchTimer);
    emojiSearchTimer=setTimeout(updateEmojiList,100);
  };
  document.addEventListener('keydown',event=>{
    if(event.key==='Escape'&&!emojiPanel.hidden){closeEmoji();}
  });

  // Voice recording: offline capture => review => explicit user-controlled upload.
  const voicePanel=E('ccVoicePanel'), voiceButton=E('ccVoiceButton');
  const voiceStatus=E('ccVoiceStatus'),voicePreview=E('ccVoicePreview');
  const voiceStop=E('ccVoiceStop'),voiceSend=E('ccVoiceSend'),voiceDiscard=E('ccVoiceDiscard');
  let recording=null, voiceBlob=null, voiceMime='', voicePreviewUrl='',voiceContext=null;
  let recordInterval=null, preparing=false;
  const audioObjectUrls=new Set();
  const voiceNow=()=>Date.now();
  function audioContextMatches(info){
    return !!(info && session===info.account && target===info.to && groupTarget===info.group);
  }
  function releaseStream(info){
    if(!info)return;
    try{info.stream.getTracks().forEach(track=>track.stop());}catch{}
  }
  function releasePreview(){
    voicePreview.pause();voicePreview.removeAttribute('src');voicePreview.load();
    if(voicePreviewUrl)URL.revokeObjectURL(voicePreviewUrl);
    voicePreviewUrl='';voiceBlob=null;voiceMime='';
  }
  function resetVoiceUI(){
    clearInterval(recordInterval);recordInterval=null;
    if(recording){const info=recording;recording=null;info.cancelled=true;
      try{if(info.rec.state!=='inactive')info.rec.stop();}catch{}
      releaseStream(info);
    }
    releasePreview();voiceContext=null;preparing=false;
    voicePanel.hidden=true;voiceStop.hidden=true;voiceSend.hidden=true;
    voicePreview.hidden=true;voiceButton.disabled=false;
    voiceButton.setAttribute('aria-pressed','false');voiceStatus.textContent='🎙️ Ready';
  }
  function cleanupVoice(){
    resetVoiceUI();
    for(const url of audioObjectUrls)URL.revokeObjectURL(url);
    audioObjectUrls.clear();
  }
  window.ccVoiceCleanup=cleanupVoice;
  function elapsed(seconds){
    const hh=Math.floor(seconds/3600);
    const mm=Math.floor((seconds%3600)/60).toString().padStart(2,'0');
    const ss=(seconds%60).toString().padStart(2,'0');
    return hh>0?`${hh}:${mm}:${ss}`:`${mm}:${ss}`;
  }
  function preferredRecordingType(){
    const types=[['audio/webm;codecs=opus','.webm'],['audio/ogg;codecs=opus','.ogg'],
      ['audio/mp4','.m4a'],['audio/webm','.webm'],['audio/ogg','.ogg']];
    return types.find(([mime])=>typeof MediaRecorder.isTypeSupported==='function'&&MediaRecorder.isTypeSupported(mime))||null;
  }
  function finishRecording(info){
    if(recording!==info)return;
    clearInterval(recordInterval);recordInterval=null;
    releaseStream(info);recording=null;
    voiceButton.setAttribute('aria-pressed','false');voiceStop.hidden=true;
    if(info.cancelled || !audioContextMatches(info)){
      resetVoiceUI();return;
    }
    const blob=new Blob(info.chunks,{type:info.mime});
    if(blob.size<16 || blob.size>VOICE_LIMIT){
      resetVoiceUI();E('sendError').textContent='Voice recording was empty or over 300 MiB. Please retry.';return;
    }
    voiceBlob=blob;voiceMime=info.mime;voiceContext=info;
    voicePreviewUrl=URL.createObjectURL(blob);
    voicePreview.src=voicePreviewUrl;voicePreview.hidden=false;voiceSend.hidden=false;
    voiceStatus.textContent=(info.reachedLimit?'📦 Size cap reached — ':'✅ Voice note ready — ')+
      elapsed(Math.floor((voiceNow()-info.started)/1000))+
      ` (${(blob.size/1048576).toFixed(1)} MiB / 300 MiB). Review or send.`;
  }
  function stopRecording(){
    if(!recording || recording.rec.state==='inactive')return;
    voiceStop.disabled=true;voiceStatus.textContent='Preparing voice note…';
    try{recording.rec.stop();}catch(error){E('sendError').textContent=error.message;resetVoiceUI();}
  }
  async function beginRecording(){
    if(!session ||(!target&&!groupTarget)){
      E('sendError').textContent='Select Vineth (or another friend/group) before recording.';return;
    }
    if(activeMediaUpload){E('sendError').textContent='Finish current upload first.';return;}
    if(!window.isSecureContext || !navigator.mediaDevices?.getUserMedia || typeof MediaRecorder==='undefined'){
      E('sendError').textContent='Microphone recording requires localhost/HTTPS and a supported browser.';return;
    }
    const format=preferredRecordingType();
    if(!format){E('sendError').textContent='Voice recording format not supported in this browser.';return;}
    const context={account:session,to:target,group:groupTarget};
    voiceButton.disabled=true;E('sendError').textContent='';
    let stream;
    try{
      stream=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true,noiseSuppression:true}});
      if(!audioContextMatches(context)){stream.getTracks().forEach(track=>track.stop());return;}
      resetVoiceUI();
      const rec=new MediaRecorder(stream,{mimeType:format[0],audioBitsPerSecond:64000});
      const info={...context,rec,stream,mime:format[0],ext:format[1],started:voiceNow(),
        bytes:0,chunks:[],cancelled:false,reachedLimit:false};
      recording=info;voicePanel.hidden=false;voiceStop.hidden=false;voiceStop.disabled=false;
      voiceSend.hidden=true;voicePreview.hidden=true;
      voiceButton.setAttribute('aria-pressed','true');voiceStatus.textContent='🔴 Recording 00:00';
      rec.ondataavailable=event=>{
        if(!event.data?.size || info.cancelled)return;
        // Keep the last complete recording chunk; never exceed 300 MiB.
        if(info.bytes+event.data.size>VOICE_LIMIT){
          info.reachedLimit=true;stopRecording();return;
        }
        info.bytes+=event.data.size;
        info.chunks.push(event.data);
        if(info.bytes>=VOICE_LIMIT){info.reachedLimit=true;stopRecording();}
      };
      rec.onerror=()=>{resetVoiceUI();E('sendError').textContent='Microphone recording failed; please retry.';};
      rec.onstop=()=>finishRecording(info);
      rec.start(1000);
      recordInterval=setInterval(()=>{
        if(recording!==info)return;
        const seconds=Math.floor((voiceNow()-info.started)/1000);
        voiceStatus.textContent='🔴 Recording '+elapsed(seconds)+
          ' · '+(info.bytes/1048576).toFixed(1)+' / 300 MiB';
      },250);
    }catch(error){
      if(stream)stream.getTracks().forEach(track=>track.stop());
      resetVoiceUI();
      E('sendError').textContent=error?.name==='NotAllowedError' ?
        'Microphone permission denied. Allow microphone in browser site settings.' :
        'Could not start microphone: '+(error.message||'Unknown error');
    }finally{voiceButton.disabled=false;}
  }
  voiceButton.onclick=()=>{
    if(recording){stopRecording();return;}
    if(voiceBlob){resetVoiceUI();return;}
    beginRecording();
  };
  voiceStop.onclick=stopRecording;
  voiceDiscard.onclick=resetVoiceUI;
  voiceSend.onclick=async()=>{
    if(preparing||!voiceBlob || !audioContextMatches(voiceContext))return;
    preparing=true;voiceSend.disabled=true;
    voiceStatus.textContent='⏳ Sending voice note…';
    try{
      const stamp=new Date().toISOString().replace(/[:.]/g,'-');
      const f=new File([voiceBlob],'Voice-'+stamp+voiceContext.ext,{type:voiceMime});
      const success=await ccUploadMediaFile(f,{voice:true});
      if(success){resetVoiceUI();}else voiceStatus.textContent='Upload failed. Retry or discard.';
    }catch(error){E('sendError').textContent=error.message;voiceStatus.textContent='Upload failed.';}
    finally{voiceSend.disabled=false;preparing=false;}
  };

  // Voice message playback: fetch only on click, with Bearer auth; no public URLs.
  window.ccRenderVoiceAttachment=(card,attachment)=>{
    const wrapper=document.createElement('div');wrapper.className='cc-voice-card';
    const title=document.createElement('strong');title.className='cc-voice-card-title';title.textContent='🎙️ Voice message';
    const size=document.createElement('small');size.textContent=(Number(attachment.size)/1024).toFixed(0)+' KB';
    const listen=document.createElement('button');listen.type='button';listen.textContent='▶ Listen';
    listen.title='Load private voice note and play';
    const player=document.createElement('audio');player.controls=true;player.preload='none';player.hidden=true;
    player.setAttribute('aria-label','Voice message playback');
    const download=document.createElement('button');download.type='button';download.textContent='↓ Download';
    download.onclick=()=>{try{requestNativeAttachmentDownload(attachment.id);}
      catch(error){E('sendError').textContent=error.message;}};
    listen.onclick=async()=>{
      if(!session)return;
      const account=session;
      listen.disabled=true;listen.textContent='⏳ Loading…';
      try{
        const res=await fetch('/api/attachments/audio',{
          method:'POST',headers:{Authorization:'Bearer '+account.token},cache:'no-store',
          body:new URLSearchParams({id:String(attachment.id)})
        });
        if(!res.ok){let message='Audio unavailable ('+res.status+').';
          try{message=(await res.json()).error||message;}catch{}
          throw new Error(message);
        }
        const blob=await res.blob();
        if(session!==account)throw new Error('Account changed.');
        if(blob.size>VOICE_LIMIT)throw new Error('Voice file exceeds 300 MiB limit.');
        const url=URL.createObjectURL(blob);audioObjectUrls.add(url);
        player.src=url;player.hidden=false;listen.hidden=true;
        try{await player.play();}catch{ /* Browser may require tapping Play after loading. */ }
      }catch(error){E('sendError').textContent=error.message;listen.textContent='▶ Retry audio';listen.disabled=false;}
    };
    wrapper.append(title,size,listen,player,download);card.append(wrapper);
  };
})();
