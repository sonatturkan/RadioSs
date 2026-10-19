(() => {
    const workspace = document.querySelector('[data-live-desk]');
    if (!workspace) return;

    const panels = Array.from(workspace.querySelectorAll('[data-workspace-panel]'));
    const selectPanel = (selected) => {
        if (!panels.includes(selected)) return;

        for (const panel of panels) {
            const active = panel === selected;
            panel.classList.toggle('is-selected', active);
            panel.querySelector('.workspace-tab')?.setAttribute('aria-pressed', String(active));
        }
    };

    // Selection follows the operator's workspace; it does not change audio
    // or hide the persistent broadcast information and other working areas.
    const selectFromEvent = (event) => {
        if (!(event.target instanceof Element)) return;
        selectPanel(event.target.closest('[data-workspace-panel]'));
    };

    workspace.addEventListener('click', selectFromEvent);
    workspace.addEventListener('focusin', selectFromEvent);
    selectPanel(panels.find((panel) => panel.classList.contains('is-selected')) || panels[0]);
})();
