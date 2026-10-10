// 章ページの右側に「この章の内容」を見出しから組み立て、読んでいる節を強調する。
// スクリプトが動かない環境でも本文はそのまま読める。
(() => {
  const toc = document.querySelector('.page-toc');
  const content = document.querySelector('.content');
  if (!toc || !content) return;

  const headings = Array.from(content.querySelectorAll('h2'));
  if (headings.length < 2) return;

  const title = document.createElement('p');
  title.className = 'page-toc-title';
  title.textContent = 'この章の内容';

  const list = document.createElement('ol');
  const links = headings.map((heading, index) => {
    if (!heading.id) heading.id = `sec-${index + 1}`;

    const link = document.createElement('a');
    link.href = `#${heading.id}`;
    const num = heading.querySelector('.sec-num');
    let text = heading.textContent.trim();
    if (num) {
      const numSpan = document.createElement('span');
      numSpan.className = 'toc-num';
      numSpan.textContent = num.textContent;
      link.append(numSpan);
      text = text.slice(num.textContent.length).trim();
    }
    link.append(text);

    const item = document.createElement('li');
    item.append(link);
    list.append(item);
    return link;
  });

  toc.append(title, list);
  toc.hidden = false;

  // ヘッダーの下を通り過ぎた最後の見出しを、読んでいる節とみなす。
  let scheduled = false;
  const update = () => {
    scheduled = false;
    let current = 0;
    headings.forEach((heading, index) => {
      if (heading.getBoundingClientRect().top <= 120) current = index;
    });
    links.forEach((link, index) => {
      if (index === current) link.setAttribute('aria-current', 'true');
      else link.removeAttribute('aria-current');
    });
  };
  window.addEventListener('scroll', () => {
    if (scheduled) return;
    scheduled = true;
    requestAnimationFrame(update);
  }, { passive: true });
  update();
})();
