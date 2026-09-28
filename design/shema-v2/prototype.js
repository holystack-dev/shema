const picker = document.querySelector('#picker');
const device = document.querySelector('#device');
const state = { page: 'home', history: [], book: 'John', chapter: 3, volume: 70,
  brightness: 70, sleep: 'Off', loop: false, swipe: true, timeout: '30 seconds',
  power: '10 minutes', theme: 'gold', version: 'NRSV', saved: false, cleared: false };
for (const name of Object.keys(screens)) {
  const option = document.createElement('option');
  option.value = name; option.textContent = name.replaceAll('-', ' '); picker.append(option);
}
function paint() {
  let svg = screens[state.page];
  if (['player', 'player-paused', 'chapters', 'chapters-2'].includes(state.page)) {
    svg = svg.replace('>John<', '>' + state.book + '<').replace('>Chapter 3<', '>Chapter ' + state.chapter + '<');
  }
  if (state.page === 'volume' || state.page === 'brightness') svg = svg.replace('>70%<', '>' + state[state.page] + '%<');
  if (state.page === 'player-options') svg = svg.replace('70% · tap to adjust', state.volume + '% · tap to adjust').replace('>Off<', '>' + state.sleep + '<').replace('>Save<', state.saved ? '>Saved<' : '>Save<');
  if (state.page === 'settings') svg = svg.replace('>70%<', '>' + state.brightness + '%<').replace('English · NRSV', state.version === 'NRSV' ? 'English · NRSV' : 'Malayalam · POC');
  if (state.page === 'settings-2') { svg = svg.replace('>Off<', '>LOOP_VALUE<').replace('>On<', state.swipe ? '>On<' : '>Off<').replace('>LOOP_VALUE<', state.loop ? '>On<' : '>Off<'); }
  if (state.page === 'settings-3') svg = svg.replace('>30 seconds<', '>' + state.timeout + '<').replace('>10 minutes<', '>' + state.power + '<');
  if (state.page === 'settings-4') svg = svg.replace('>Champagne<', state.theme === 'gold' ? '>Champagne<' : '>Sage<');
  svg = svg.replaceAll('>NRSV<', '>' + state.version + '<');
  if (state.theme === 'sage') svg = svg.replaceAll('#E8BE78', '#A9CBB2');
  device.innerHTML = svg; picker.value = state.page;
  if (state.page.startsWith('chapters')) device.querySelectorAll('a[data-screen="player"]').forEach(a => { const selected = Number(a.querySelector('text').textContent) === state.chapter; a.querySelector('rect').setAttribute('fill', selected ? (state.theme === 'sage' ? '#A9CBB2' : '#E8BE78') : '#17212B'); a.querySelector('text').setAttribute('fill', selected ? '#080D13' : '#F7F1E6'); });
  device.querySelectorAll('a').forEach(a => {
    const label = [...a.querySelectorAll('text')].map(t => t.textContent).join(', ');
    a.setAttribute('aria-label', label || ({back:'Back', more:'More'}[a.dataset.screen] || a.dataset.screen));
    a.setAttribute('tabindex', '0');
  });
}
function navigate(page, remember = true) {
  if (page === 'back') return goBack();
  if (!screens[page]) return;
  if (remember && state.page !== page) state.history.push(state.page);
  state.page = page; paint();
}
function goBack() { state.page = state.history.pop() || 'home'; paint(); }
function act(target, label = '', anchor) {
  const page = state.page;
  if (/^(volume|brightness)-(up|down)$/.test(target)) {
    const [key, direction] = target.split('-');
    state[key] = Math.max(0, Math.min(100, state[key] + (direction === 'up' ? 10 : -10))); return paint();
  }
  if (page === 'settings-2' && target === 'settings-2') {
    if (label.includes('Loop')) state.loop = !state.loop; else state.swipe = !state.swipe;
    return paint();
  }
  if (target.startsWith('set-timeout-')) {state.timeout = ({'30':'30 seconds','60':'1 minute','off':'Never'})[target.slice(12)]; return goBack();}
  if (target.startsWith('set-power-')) {state.power = ({'10':'10 minutes','30':'30 minutes','off':'Off'})[target.slice(10)]; return goBack();}
  if (target.startsWith('set-theme-')) {state.theme = target.slice(10); return goBack();}
  if (target === 'clear-history') {state.cleared = true; return navigate('empty-recent');}
  if (target === 'recent' && state.cleared) return navigate('empty-recent');
  if (page === 'player-options' && target === 'favourites') {state.saved = !state.saved; return paint();}
  if (page === 'sleep' && target === 'player') {state.sleep = label.includes('15') ? '15 minutes' : label.includes('30') ? '30 minutes' : 'Off'; return goBack();}
  if (page === 'versions' && target === 'home' && !label.includes('Done')) {state.version = label.includes('Malayalam') ? 'POC' : 'NRSV'; return goBack();}
  if (target === 'chapters' && /books|book-list/.test(page)) {state.book = anchor.querySelector('text').textContent; state.chapter = 1;}
  if (/^chapters/.test(page) && target === 'player') state.chapter = Number(anchor.querySelector('text').textContent);
  if ((page === 'player' || page === 'player-paused') && target === 'player') {
    const anchors = [...device.querySelectorAll('a')]; const index = anchors.indexOf(anchor);
    if (index === 0) state.chapter = Math.max(1, state.chapter - 1);
    if (index === 2) state.chapter = Math.min(({John:21,Acts:28,Romans:16,'1 Corinthians':16,Genesis:50,Exodus:40,Leviticus:27,Numbers:36})[state.book] || 150, state.chapter + 1);
  }
  if (page === 'saved-edit' && target === 'saved') {state.saved = false; return navigate('empty-saved');}
  navigate(target, !(page.startsWith('player') && ['player', 'player-paused'].includes(target)));
}
device.addEventListener('click', event => {
  const anchor = event.target.closest('[data-screen]');
  if (!anchor) { if (state.page === 'splash') navigate('home'); return; }
  event.preventDefault(); act(anchor.dataset.screen, anchor.textContent, anchor);
});
device.addEventListener('keydown', event => {
  if (event.key === 'Escape') goBack();
  if (event.key === ' ' && event.target.matches('a')) {event.preventDefault(); event.target.dispatchEvent(new MouseEvent('click', {bubbles:true}));}
});
picker.onchange = () => navigate(picker.value);
for (const name of featured) {
  const figure = document.createElement('figure');
  figure.innerHTML = '<img src="' + name + '.svg" alt="' + name.replaceAll('-', ' ') + ' concept"><figcaption>' + name.replaceAll('-', ' ') + '</figcaption>';
  document.querySelector('#gallery').append(figure);
}
navigate(screens.splash ? 'splash' : 'home', false);
