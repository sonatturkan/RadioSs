(() => {
    for (const bank of document.querySelectorAll('[data-pad-bank]')) {
        const pages = Array.from(bank.querySelectorAll('.pad-page[data-pad-page]'))
            .filter((page) => ['1', '2'].includes(page.dataset.padPage));
        const buttons = Array.from(bank.querySelectorAll('button[data-pad-page-button]'));
        const status = bank.querySelector('[data-pad-page-status]');
        if (!pages.length) continue;

        const selectPage = (number) => {
            const selected = pages.find((page) => page.dataset.padPage === number);
            if (!selected) return;

            for (const page of pages) page.hidden = page !== selected;
            for (const button of buttons) {
                button.setAttribute('aria-pressed', String(button.dataset.padPageButton === number));
            }
            bank.dataset.activePage = number;
            if (status) status.textContent = `${number} / 2`;
        };

        for (const button of buttons) {
            button.addEventListener('click', () => {
                if (!button.disabled) selectPage(button.dataset.padPageButton);
            });
        }

        const initial = buttons.find((button) => button.getAttribute('aria-pressed') === 'true');
        selectPage(pages.some((page) => page.dataset.padPage === initial?.dataset.padPageButton)
            ? initial.dataset.padPageButton
            : pages[0].dataset.padPage);
    }
})();
