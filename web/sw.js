/* ChatConnect PWA: public static shell ONLY. No messages, auth, APIs or uploads. */
const SHELL_CACHE='chatconnect-static-pwa-v3-calls-20261108';
const PREFIX='chatconnect-static-pwa-';
const SHELL=['/','/style.css','/app.js','/cc-calls.js','/cc-emoji-data.js','/cc-voice-emoji.js',
  '/pwa-icon-192.png','/pwa-icon-512.png','/pwa-icon-180.png','/manifest.webmanifest'];
const STATIC=new Set(SHELL);
self.addEventListener('install',event=>{
  event.waitUntil((async()=>{
    const cache=await caches.open(SHELL_CACHE);
    for(const path of SHELL){
      try{
        const response=await fetch(new Request(path,{cache:'reload',credentials:'same-origin'}));
        if(response.ok && response.type==='basic')await cache.put(path,response.clone());
      }catch(error){console.warn('Static shell resource unavailable:',path);}
    }
    await self.skipWaiting();
  })());
});
self.addEventListener('activate',event=>{
  event.waitUntil((async()=>{
    const keys=await caches.keys();
    await Promise.all(keys.filter(key=>key.startsWith(PREFIX)&&key!==SHELL_CACHE)
      .map(key=>caches.delete(key)));
    await self.clients.claim();
  })());
});
self.addEventListener('fetch',event=>{
  const request=event.request;
  if(request.method!=='GET')return;
  const url=new URL(request.url);
  if(url.origin!==self.location.origin)return;
  if(url.pathname.startsWith('/api/') || url.pathname.startsWith('/uploads/'))return;
  const path=request.mode==='navigate'?'/':url.pathname;
  if(!STATIC.has(path))return; // all authenticated/private routes bypass this worker
  event.respondWith((async()=>{
    try{
      const response=await fetch(request);
      if(response.ok && response.type==='basic'){
        // Never cache responses with credentials, even if URL matches a shell path.
        if(!request.headers.has('Authorization')){
          const cache=await caches.open(SHELL_CACHE);
          await cache.put(path,response.clone());
        }
      }
      return response;
    }catch(error){
      const cached=await caches.match(path,{cacheName:SHELL_CACHE});
      if(cached)return cached;
      throw error;
    }
  })());
});
self.addEventListener('notificationclick',event=>{
  const data=event.notification.data||{};
  const kind=['friend','group'].includes(data.kind)?data.kind:'';
  const id=typeof data.id==='string'?data.id:'';
  event.notification.close();
  event.waitUntil((async()=>{
    const windows=await self.clients.matchAll({type:'window',includeUncontrolled:true});
    const open=windows.find(client=>new URL(client.url).origin===self.location.origin);
    if(open){
      await open.focus();
      if(kind&&id)open.postMessage({type:'cc-pwa-open',kind,id});
    }else await self.clients.openWindow('/');
  })());
});
