(function () {
  const volumes = {
    ru: [
      { key: "index", href: "index.html", label: "Главная" },
      { key: "reference", href: "reference.html", label: "Reference" },
      { key: "provisioning", href: "provisioning.html", label: "Provisioning" },
      { key: "mqtt", href: "mqtt-cloud.html", label: "MQTT" },
      { key: "storage", href: "storage-ota.html", label: "Storage/OTA" },
      { key: "runbooks", href: "field-runbooks.html", label: "Runbooks" }
    ],
    en: [
      { key: "index", href: "index.html", label: "Home" },
      { key: "reference", href: "reference.html", label: "Reference" },
      { key: "provisioning", href: "provisioning.html", label: "Provisioning" },
      { key: "mqtt", href: "mqtt-cloud.html", label: "MQTT" },
      { key: "storage", href: "storage-ota.html", label: "Storage/OTA" },
      { key: "runbooks", href: "field-runbooks.html", label: "Runbooks" }
    ]
  };

  const html = document.documentElement;
  const body = document.body;
  const ruBtn = document.getElementById("lang-ru");
  const enBtn = document.getElementById("lang-en");
  const pageKey = body.dataset.pageKey || "";
  const sectionNav = document.getElementById("page-nav");
  const saved = localStorage.getItem("docs-lang") || "en";

  function renderVolumeLinks(lang) {
    if (!sectionNav) {
      return;
    }

    sectionNav.innerHTML = volumes[lang]
      .map((item) => {
        const activeClass = item.key === pageKey ? " active" : "";
        return `<li><a class="volume-link${activeClass}" href="${item.href}">${item.label}</a></li>`;
      })
      .join("");
  }

  function setLang(lang) {
    html.classList.remove("lang-ru", "lang-en");
    html.classList.add(lang === "ru" ? "lang-ru" : "lang-en");
    html.lang = lang;
    localStorage.setItem("docs-lang", lang);

    if (ruBtn) {
      ruBtn.classList.toggle("active", lang === "ru");
    }
    if (enBtn) {
      enBtn.classList.toggle("active", lang === "en");
    }

    if (body.dataset.titleRu && body.dataset.titleEn) {
      document.title = lang === "ru" ? body.dataset.titleRu : body.dataset.titleEn;
    }

    renderVolumeLinks(lang);
  }

  if (ruBtn) {
    ruBtn.addEventListener("click", function () { setLang("ru"); });
  }
  if (enBtn) {
    enBtn.addEventListener("click", function () { setLang("en"); });
  }

  setLang(saved);
}());
