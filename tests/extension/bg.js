const log = [];
async function note(k, v){ const r = (await browser.storage.local.get('log')).log || []; r.push(k+':'+v); await browser.storage.local.set({log:r}); }
browser.runtime.onInstalled.addListener(d => note('installed', d.reason));
browser.runtime.onMessage.addListener(async (msg, sender) => {
  await note('msg', msg.hello + '@' + (sender.tab ? sender.tab.url : 'notab'));
  const tabs = await browser.tabs.query({active:true});
  await note('tabs', tabs.length);
  // bg -> content roundtrip
  const back = await browser.tabs.sendMessage(sender.tab.id, {ping:1});
  await note('pingback', JSON.stringify(back));
  return {reply:'hi from bg', id: browser.runtime.id};
});
browser.storage.onChanged.addListener((c,a)=>{ if(!c.log) note('changed', a+':'+Object.keys(c)); });
