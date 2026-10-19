(() => {
    const clock = document.querySelector('.header-clock-summary');
    if (!clock) return;

    const hourHand = clock.querySelector('[data-clock-hour]');
    const minuteHand = clock.querySelector('[data-clock-minute]');
    const secondHand = clock.querySelector('[data-clock-second]');
    const digitalTime = clock.querySelector('#header-clock-time');
    const calendar = clock.querySelector('#header-calendar');
    const dateFormatter = new Intl.DateTimeFormat('tr-TR', {
        timeZone: 'UTC', day: 'numeric', month: 'long', year: 'numeric'
    });
    const weekdayFormatter = new Intl.DateTimeFormat('tr-TR', {
        timeZone: 'UTC', weekday: 'long'
    });
    let timer;
    let previousDate;

    function updateClock() {
        clearTimeout(timer);
        const now = new Date();
        const hours = now.getUTCHours();
        const minutes = now.getUTCMinutes();
        const seconds = now.getUTCSeconds();
        const isoDate = now.toISOString();
        const dateKey = isoDate.slice(0, 10);

        hourHand.setAttribute('transform', `rotate(${(hours % 12) * 30 + minutes * .5 + seconds / 120} 60 60)`);
        minuteHand.setAttribute('transform', `rotate(${minutes * 6 + seconds * .1} 60 60)`);
        secondHand.setAttribute('transform', `rotate(${seconds * 6} 60 60)`);
        digitalTime.textContent = [hours, minutes, seconds].map(value => String(value).padStart(2, '0')).join(':');
        digitalTime.setAttribute('datetime', isoDate);

        if (dateKey !== previousDate) {
            calendar.textContent = `${dateFormatter.format(now)} · ${weekdayFormatter.format(now)}`;
            calendar.setAttribute('datetime', dateKey);
            previousDate = dateKey;
        }

        if (!document.hidden) {
            timer = setTimeout(updateClock, 1000 - now.getUTCMilliseconds());
        }
    }

    document.addEventListener('visibilitychange', updateClock);
    updateClock();
})();
