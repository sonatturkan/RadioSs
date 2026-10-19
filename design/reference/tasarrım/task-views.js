(() => {
  'use strict';
  const nav = document.getElementById('task-profile-nav');
  if (!nav) return;
  const q = (selector, root = document) => root.querySelector(selector);
  const qa = (selector, root = document) => Array.from(root.querySelectorAll(selector));
  const field = (form, name) => form.elements.namedItem(name);
  const value = (form, name) => String(field(form, name)?.value || '').trim();
  const setFields = (form, values) => { Object.entries(values).forEach(([name, data]) => { const input = field(form, name); if (!input) return; if (input.type === 'checkbox') input.checked = Boolean(data); else input.value = String(data ?? ''); input.setCustomValidity?.(''); }); };
  const storageKey = 'radios-workspace-preferences-v1';
  const profileLabels = { broadcast: 'Yayın', playlist: 'Playlist', automation: 'Otomasyon', dj: 'DJ', emergency: 'Acil', advert: 'Reklam', technical: 'Teknik yönetim' };
  let preferences = { activeProfile: 'broadcast', defaultProfile: 'last', textSize: 'normal' };
  try { const saved = JSON.parse(localStorage.getItem(storageKey) || 'null'); if (saved && typeof saved === 'object') { if (Object.hasOwn(profileLabels, saved.activeProfile)) preferences.activeProfile = saved.activeProfile; if (saved.defaultProfile === 'last' || Object.hasOwn(profileLabels, saved.defaultProfile)) preferences.defaultProfile = saved.defaultProfile; if (['compact', 'normal', 'large'].includes(saved.textSize)) preferences.textSize = saved.textSize; } } catch (_) { /* File-mode browsers may restrict storage. */ }
  let storageAvailable = true;
  const persist = () => { try { localStorage.setItem(storageKey, JSON.stringify(preferences)); storageAvailable = true; } catch (_) { storageAvailable = false; } };
  const applyTextSize = (textSize = preferences.textSize) => { const scale = { compact: .9, normal: 1, large: 1.15 }[textSize] || 1; qa('.task-view').forEach((view) => view.style.setProperty('--task-font-scale', String(scale))); const live = document.getElementById('live-task-view'); if (live) live.dataset.taskTextSize = textSize; document.dispatchEvent(new CustomEvent('radios:preferences-change', { detail: { textSize, defaultProfile: preferences.defaultProfile } })); };
  const revealProfileTab = (tab) => {
    const strip = q('.task-profile-tabs', nav);
    if (!strip || !tab) return;
    const bounds = strip.getBoundingClientRect();
    const tabBounds = tab.getBoundingClientRect();
    if (tabBounds.right > bounds.right - 12) strip.scrollLeft += tabBounds.right - bounds.right + 12;
    else if (tabBounds.left < bounds.left + 12) strip.scrollLeft += tabBounds.left - bounds.left - 12;
  };
  const activateProfile = (profile, focus = false, save = true) => {
    if (!Object.hasOwn(profileLabels, profile)) return;
    qa('[data-task-profile]', nav).forEach((tab) => { const active = tab.dataset.taskProfile === profile; tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1; const view = document.getElementById(tab.getAttribute('aria-controls')); if (view) view.hidden = !active; if (active && focus) tab.focus(); });
    revealProfileTab(q('[aria-selected="true"]', nav));
    preferences.activeProfile = profile; if (save) persist();
    const footer = document.getElementById('footer-profile-name'); if (footer) footer.textContent = profileLabels[profile];
    document.dispatchEvent(new CustomEvent('radios:profile-change', { detail: { profile, label: profileLabels[profile] } }));
  };
  nav.addEventListener('click', (event) => { const tab = event.target.closest('[data-task-profile]'); if (tab) activateProfile(tab.dataset.taskProfile); });
  nav.addEventListener('focusin', (event) => revealProfileTab(event.target.closest('[data-task-profile]')));
  nav.addEventListener('keydown', (event) => { if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return; const tabs = qa('[data-task-profile]', nav); const current = tabs.indexOf(event.target); if (current < 0) return; event.preventDefault(); const next = event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + tabs.length) % tabs.length; activateProfile(tabs[next].dataset.taskProfile, true); });
  // Open the temporary queue without dropping the playlist item on a tab.
  let emergencyHoverTimer = null;
  const cancelEmergencyHover = () => { window.clearTimeout(emergencyHoverTimer); emergencyHoverTimer = null; };
  nav.addEventListener('dragover', (event) => {
    const tab = event.target.closest('[data-task-profile="emergency"]');
    const compatible = Array.from(event.dataTransfer?.types || []).includes('application/x-radios-programme-items');
    if (!tab || !compatible) { cancelEmergencyHover(); return; }
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
    if (tab.getAttribute('aria-selected') === 'true' || emergencyHoverTimer !== null) return;
    emergencyHoverTimer = window.setTimeout(() => { emergencyHoverTimer = null; activateProfile('emergency'); }, 450);
  });
  nav.addEventListener('dragleave', (event) => {
    const tab = event.target.closest('[data-task-profile="emergency"]');
    if (tab && !tab.contains(event.relatedTarget)) cancelEmergencyHover();
  });
  nav.addEventListener('drop', cancelEmergencyHover);
  document.addEventListener('dragend', cancelEmergencyHover);
  document.getElementById('footer-profile-button')?.addEventListener('click', () => q('[aria-selected="true"]', nav)?.focus());
  window.addEventListener('resize', () => revealProfileTab(q('[aria-selected="true"]', nav)));

  const technicalForm = q('[data-technical-form]');
  setFields(technicalForm, { textSize:preferences.textSize, defaultProfile:preferences.defaultProfile });
  field(technicalForm,'textSize').addEventListener('change', () => { applyTextSize(value(technicalForm,'textSize')); q('[data-technical-status]').textContent = ''; });
  technicalForm.addEventListener('submit', (event) => { event.preventDefault(); if (!technicalForm.reportValidity()) return; preferences.textSize = value(technicalForm,'textSize'); preferences.defaultProfile = value(technicalForm,'defaultProfile'); persist(); applyTextSize(); field(technicalForm,'secret').value = ''; q('[data-technical-status]').textContent = storageAvailable ? 'Kaydedildi.' : 'Kalıcı kayıt yapılamadı.'; });
  q('[data-task-reset]').addEventListener('click', () => { technicalForm.reset(); field(technicalForm,'secret').value = ''; preferences = { activeProfile:'broadcast', defaultProfile:'broadcast', textSize:'normal' }; setFields(technicalForm, { textSize:'normal', defaultProfile:'broadcast' }); persist(); applyTextSize(); q('[data-technical-status]').textContent = 'Varsayılanlar yüklendi.'; activateProfile('broadcast', true); });
  applyTextSize(); activateProfile(preferences.defaultProfile === 'last' ? preferences.activeProfile : preferences.defaultProfile, false, false);
})();
