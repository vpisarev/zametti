#!/usr/bin/env osascript -l JavaScript
// Манифест Apple Notes для `zametti-store import --apple-manifest`.
//
// Выгружает JSON-массив записей { folder, title, created, modified } по всем
// заметкам всех учёток. Тел заметок не читает — только имена и даты; поэтому
// работает быстро и не спотыкается о защищённые паролем заметки.
//
//   osascript -l JavaScript apple-notes-manifest.js \
//       [--prefix "Apple Notes"] [--default-folder Notes] [--skip "Имя папки"] \
//       > manifest.json
//
// --prefix          приставка к каждому пути папки — так, как папки лежат в
//                   конвертированном дереве (у типичного конвертера это
//                   "Apple Notes"). По умолчанию пусто.
// --default-folder  папка Apple по умолчанию (обычно "Notes" или «Заметки»):
//                   её заметки конвертеры кладут в корень, без подкаталога.
//                   Указанная папка отображается в сам префикс.
// --skip            папка, которую не выгружать; можно повторять. «Недавно
//                   удалённые» пропускаются всегда.
//
// Ход выполнения печатается в stderr (console.log в osascript идёт туда),
// сам манифест — единственное, что попадает в stdout.

function run(argv) {
    let prefix = '';
    let defaultFolder = 'Notes';
    const skipNames = new Set([
        'Recently Deleted', 'Недавно удалённые', 'Недавно удаленные',
    ]);
    for (let i = 0; i < argv.length; ++i) {
        const a = argv[i];
        if (a === '--prefix') prefix = argv[++i] ?? '';
        else if (a === '--default-folder') defaultFolder = argv[++i] ?? 'Notes';
        else if (a === '--skip') skipNames.add(argv[++i] ?? '');
        else throw new Error('неизвестный аргумент: ' + a);
    }

    // Даты — ISO-8601 UTC с точностью до секунды: YYYY-MM-DDTHH:MM:SSZ.
    const iso = (d) => d.toISOString().replace(/\.\d+Z$/, 'Z');
    const joinPath = (a, b) => (a === '' ? b : b === '' ? a : a + '/' + b);

    const Notes = Application('Notes');
    const out = [];
    const seenFolders = new Set();

    // Путь папки — вверх по цепочке контейнеров, а не рекурсией вниз: в
    // некоторых версиях macOS `folders` учётки отдаёт ВСЕ папки плоско, и
    // рекурсия обходила бы вложенные дважды.
    const partsOf = (folder) => {
        const parts = [folder.name()];
        let f = folder;
        for (;;) {
            let container;
            try {
                container = f.container();
                if (container.class() !== 'folder') break;
            } catch (e) {
                break;
            }
            f = container;
            parts.unshift(f.name());
        }
        return parts;
    };

    const harvest = (folder) => {
        let id;
        try {
            id = folder.id();
        } catch (e) {
            id = null;
        }
        if (id !== null) {
            if (seenFolders.has(id)) return;
            seenFolders.add(id);
        }

        const parts = partsOf(folder);
        if (parts.some((p) => skipNames.has(p))) {
            console.log('пропуск: ' + parts.join('/'));
            return;
        }
        // Папка по умолчанию — в корень: конвертеры не заводят под неё
        // подкаталог. Только сама, не её тёзки в глубине.
        const own = parts.length === 1 && parts[0] === defaultFolder
                        ? ''
                        : parts.join('/');
        const at = joinPath(prefix, own);

        // Пакетные геттеры на порядок быстрее пообъектных; редкий откат — по
        // одной заметке.
        let titles;
        let created;
        let modified;
        try {
            titles = folder.notes.name();
            created = folder.notes.creationDate();
            modified = folder.notes.modificationDate();
        } catch (e) {
            titles = [];
            created = [];
            modified = [];
            const notes = folder.notes();
            for (const n of notes) {
                titles.push(n.name());
                created.push(n.creationDate());
                modified.push(n.modificationDate());
            }
        }
        for (let i = 0; i < titles.length; ++i) {
            out.push({
                folder: at,
                title: titles[i],
                created: iso(created[i]),
                modified: iso(modified[i]),
            });
        }
        console.log((at === '' ? '(корень)' : at) + ': ' + titles.length);

        let subs;
        try {
            subs = folder.folders();
        } catch (e) {
            subs = [];
        }
        for (const sub of subs) harvest(sub);
    };

    const accounts = Notes.accounts();
    for (const account of accounts) {
        console.log('учётка: ' + account.name());
        const tops = account.folders();
        for (const f of tops) harvest(f);
    }

    console.log('всего заметок: ' + out.length);
    return JSON.stringify(out, null, 1);
}
