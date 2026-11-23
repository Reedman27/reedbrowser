document.title = 'ext-ran';
const d = document.createElement('div'); d.id='reed-ext-mark'; d.style.color='#fff'; d.textContent='start'; document.body.appendChild(d);
browser.runtime.onMessage.addListener((m)=>Promise.resolve({pong:m.ping, from:'content'}));
try {
browser.runtime.sendMessage({hello:'world'}).then(r=>{ d.textContent = 'reply:'+JSON.stringify(r); browser.storage.local.set({fromContent:1}); }, e=>{ d.textContent='ERR '+e; });
} catch(e){ d.textContent='THROW '+e; }
