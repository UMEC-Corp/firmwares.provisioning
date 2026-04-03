const content = {
  ru: {
    navLabel: "Разделы документации",
    heroKicker: "ESP-IDF компонент",
    heroTitle: "ESP-IDF Connectivity Component for UMEC Space IoT Cloud.",
    heroLead:
      "Компонент реализует базовый облачный контур устройства: provisioning, хранение конфигурации, авторизацию, MQTT-связь, очереди телеметрии, удаленную конфигурацию, OTA и локальное поведение кнопки и индикации.",
    heroNote:
      "Главная страница дает архитектурный обзор. Подробные прикладные тома по BLE, MQTT, OTA, справочным таблицам и диагностике вынесены на отдельные страницы ниже.",
    volumesTitle: "Подробные разделы",
    volumesLead:
      "Используйте эти страницы как подробную техническую документацию по отдельным подсистемам компонента.",
    volumes: [
      {
        title: "Reference",
        href: "reference.html",
        description: "NVS keys, MQTT topics, LED/button contract и runtime limits."
      },
      {
        title: "Provisioning",
        href: "provisioning.html",
        description: "BLE contract, bootstrap-команды, device-code flow и recovery semantics."
      },
      {
        title: "MQTT",
        href: "mqtt-cloud.html",
        description: "Connection lifecycle, route handling, publish envelopes и очереди."
      },
      {
        title: "Storage/OTA",
        href: "storage-ota.html",
        description: "Persistence model, valid time, remote config refresh и OTA path."
      },
      {
        title: "Runbooks",
        href: "field-runbooks.html",
        description: "Диагностика online state, telemetry, provisioning reset и OTA."
      }
    ],
    sections: [
      {
        id: "purpose",
        title: "1. Назначение компонента",
        meta: "Роль компонента в составе устройства",
        tags: ["purpose", "umec", "integration"],
        paragraphs: [
          "Компонент предназначен для встраивания в ESP32-устройство как готовый слой связи с UMEC Space IoT Cloud. Прикладная логика изделия не должна заново реализовывать подключение к облаку, авторизацию, MQTT runtime и OTA orchestration.",
          "Компонент берет на себя повторяемую интеграционную работу: локальный provisioning, прием и хранение сетевых и облачных параметров, публикацию состояния устройства и прием команд из облака."
        ],
        bullets: [
          "Ориентирован на ESP-IDF и семейство ESP32.",
          "Разделяет интеграционную логику и продуктовую логику устройства.",
          "Внешняя точка входа: `core::init()`."
        ]
      },
      {
        id: "public-surface",
        title: "2. Публичная поверхность",
        meta: "Что приложение передает в компонент и что наблюдает снаружи",
        tags: ["api", "entrypoint", "surface"],
        paragraphs: [
          "Публичный заголовок компонента находится в `components/core/include/core.h`. Через `CoreHardwareConfig` приложение задает пины кнопки и светодиодов, после чего передает компоненту владение connection lifecycle.",
          "Снаружи компонент наблюдается через BLE provisioning contract, MQTT topics, LED-индикацию, поведение кнопки, локально хранимую конфигурацию и OTA path."
        ],
        bullets: [
          "Board mapping задается приложением один раз при старте.",
          "Build identity задается через `core_build_info.h`.",
          "Product code расширяет устройство через producers и domain logic, не вмешиваясь в transport internals."
        ]
      },
      {
        id: "architecture",
        title: "3. Архитектура и модули",
        meta: "Границы ответственности внутри core",
        tags: ["architecture", "modules", "boundaries"],
        paragraphs: [
          "`core_app.cpp` собирает lifecycle и orchestration. `core_ble.cpp` отвечает за provisioning transport. `core_mqtt.cpp` владеет transport session и publish queue. `core_mqtt_process.cpp` содержит route handling. `core_http.cpp` обслуживает remote config, time sync и OTA. `core_storage.cpp` владеет persisted state. `core_data_queue.cpp` отвечает за логическую очередь телеметрии. `core_button_led.cpp` замыкает локальное UX.",
          "Такое разделение помогает безопасно расширять компонент: transport, persistence, local UX и semantic command handling не смешиваются в одном файле."
        ],
        bullets: [
          "Transport отделен от route-specific behavior.",
          "Persistence отделена от временных runtime flags.",
          "Local UX не владеет сетевой логикой."
        ]
      },
      {
        id: "runtime-state",
        title: "4. RuntimeState и синхронизация",
        meta: "Общее состояние компонента",
        tags: ["runtime", "state", "concurrency"],
        paragraphs: [
          "`RuntimeState` объединяет shared state компонента: persisted config, transport queues, MQTT handle, connectivity flags, provisioning state, OTA guard, LED mode и hardware ownership.",
          "Доступ к общим структурам разделен между mutex-ами и atomics: это позволяет держать конфигурацию, transport queues и connection flags в предсказуемом состоянии."
        ],
        bullets: [
          "`RuntimeConfig` хранит только долгоживущие параметры.",
          "Временные connection flags живут отдельно от persisted state.",
          "`ota_in_progress` и connection flags используются как lifecycle guards."
        ]
      },
      {
        id: "provisioning",
        title: "5. Provisioning и авторизация",
        meta: "BLE bootstrap и device-code flow",
        tags: ["ble", "provisioning", "auth"],
        paragraphs: [
          "Provisioning path построен вокруг BLE и поддерживает несколько типов входных команд: Wi-Fi bootstrap, передачу cloud endpoints и device-code authorization flow.",
          "Чувствительные операции не исполняются прямо в BLE callback context: данные сначала попадают в очередь, а затем обрабатываются в обычной задаче, где уже возможны JSON parsing, HTTP requests, NVS writes и reconnect side effects."
        ],
        bullets: [
          "`SERVER_DATA` сохраняет адреса внешних сервисов.",
          "`GET_TOKEN` и `CHECK_AUTH` обслуживают user-code authorization flow.",
          "Reset provisioning state доступен через кнопку и MQTT route `reset`."
        ]
      },
      {
        id: "mqtt-runtime",
        title: "6. Wi-Fi, MQTT и команды облака",
        meta: "Connection lifecycle и route model",
        tags: ["wifi", "mqtt", "routes"],
        paragraphs: [
          "После восстановления конфигурации устройство проходит Wi-Fi association, cloud authorization и MQTT connect. При изменении состояния компонент пересобирает transport session в контролируемом порядке, не полагаясь на неявное состояние старого клиента.",
          "Cloud-to-device contract сосредоточен в route table. Сейчас используются как минимум `upgrade`, `reboot`, `reset` и `target-temp`.",
          "MQTT subscriptions поднимаются централизованно из этой же route model через `subscribe_core_mqtt_routes()`, поэтому добавление новой команды начинается с таблицы маршрутов, а не с transport callback."
        ],
        bullets: [
          "`core_mqtt.cpp` остается transport-only слоем.",
          "`core_mqtt_process.cpp` владеет semantic command handling.",
          "Status snapshot и device-data публикуются в отдельные MQTT endpoints."
        ]
      },
      {
        id: "telemetry",
        title: "7. Телеметрия и dataQueue",
        meta: "Как компонент публикует данные в облако",
        tags: ["telemetry", "dataqueue", "thresholds"],
        paragraphs: [
          "`dataQueue` принимает semantic items, а не raw MQTT payloads. Она строит логический ключ, выполняет upsert, применяет threshold policy и только затем передает данные в publish layer.",
          "Такой подход ограничивает рост памяти и упрощает прикладному коду публикацию собственных измерений и состояний."
        ],
        bullets: [
          "Telemetry producers не зависят от topic naming.",
          "Threshold rules централизованы в `core_thresholds.h` и применяются в порядке сверху вниз по exact/prefix match.",
          "Очередь телеметрии и MQTT transport queue ограничены по памяти."
        ]
      },
      {
        id: "storage-maintenance",
        title: "8. Хранение, время и OTA",
        meta: "Persistence model и фоновые задачи обслуживания",
        tags: ["storage", "time", "ota", "maintenance"],
        paragraphs: [
          "Persisted state разделен по namespaces и хранит только то, что должно переживать reboot: Wi-Fi credentials, cloud endpoints, access tokens, device-code fields и данные восстановления времени.",
          "Фоновая задача обслуживания периодически выполняет time sync, token refresh, remote config refresh и при необходимости поддерживает OTA lifecycle."
        ],
        bullets: [
          "Пустые значения очищают NVS keys.",
          "Telemetry gated until valid time is restored or synchronized.",
          "OTA path переключает boot partition только после успешного завершения записи."
        ]
      },
      {
        id: "local-ux",
        title: "9. Кнопка и индикация",
        meta: "Локально наблюдаемое поведение устройства",
        tags: ["button", "led", "local-ux"],
        paragraphs: [
          "Светодиоды кодируют фазу жизненного цикла устройства: provisioning, Wi-Fi connect, ожидание токена, MQTT connect, online state и error state. Это делает локальную диагностику возможной даже без внешних инструментов.",
          "Кнопка используется как recovery input. Destructive reset принимается на отпускании после достаточно долгого удержания, что снижает риск случайного сброса."
        ],
        bullets: [
          "`ButtonHold` используется как явное подтверждение удержания.",
          "Reset threshold сейчас равен `>= 10000 ms`.",
          "LED behavior выводится через явный `LedMode` contract."
        ]
      },
      {
        id: "operations",
        title: "10. Эксплуатация и диагностика",
        meta: "Что важно для интеграции и сопровождения",
        tags: ["operations", "diagnostics", "field"],
        paragraphs: [
          "При интеграции важно опираться на наблюдаемые surfaces компонента: LED modes, MQTT status publication, device-data, route handling, remote config logs и OTA outcomes.",
          "Для полевой эксплуатации устройство проходит повторяемые фазы: unprovisioned boot, Wi-Fi association, authorization, MQTT online, background maintenance и при необходимости controlled recovery."
        ],
        bullets: [
          "Provisioning, authorization и MQTT transport входят в сам компонент.",
          "OTA и reset относятся к privileged operational paths.",
          "Подробные справочные таблицы и runbooks вынесены на отдельные страницы."
        ],
        table: {
          headers: ["Свойство", "Наблюдаемое поведение", "Источник"],
          rows: [
            ["MQTT transport queue", "Ограничение 64 сообщений", "core_mqtt.cpp"],
            ["Logical telemetry queue", "Upsert по method/device/parameter", "core_data_queue.cpp"],
            ["Button recovery threshold", "Сброс на отпускании после >= 10000 ms", "core_button_led.cpp"],
            ["Periodic maintenance cadence", "Синхронизация времени и remote config примерно раз в 24 часа", "core_app.cpp"],
            ["OTA execution guard", "Одновременно разрешена только одна OTA-сессия", "core_http.cpp / core_internal.h"]
          ]
        }
      }
    ]
  },
  en: {
    navLabel: "Documentation",
    heroKicker: "ESP-IDF Component",
    heroTitle: "ESP-IDF Connectivity Component for UMEC Space IoT Cloud.",
    heroLead:
      "The component implements the device cloud baseline: provisioning, persisted configuration, authorization, MQTT communication, telemetry queues, remote configuration, OTA, and local button and LED behavior.",
    heroNote:
      "This page provides the architectural overview. Detailed volumes for BLE, MQTT, OTA, reference tables, and troubleshooting are linked below.",
    volumesTitle: "Detailed Volumes",
    volumesLead:
      "Use these pages as the detailed technical documentation for individual subsystems of the component.",
    volumes: [
      {
        title: "Reference",
        href: "reference.html",
        description: "NVS keys, MQTT topics, LED/button contract, and runtime limits."
      },
      {
        title: "Provisioning",
        href: "provisioning.html",
        description: "BLE contract, bootstrap commands, device-code flow, token persistence, and release-driven recovery."
      },
      {
        title: "MQTT",
        href: "mqtt-cloud.html",
        description: "Connection lifecycle, proactive token refresh, 3/5 recovery escalation, and queue layers."
      },
      {
        title: "Storage/OTA",
        href: "storage-ota.html",
        description: "Persistence model, valid time, remote config refresh, and OTA execution."
      },
      {
        title: "Runbooks",
        href: "field-runbooks.html",
        description: "Diagnostics for online state, telemetry, provisioning reset, and OTA."
      }
    ],
    sections: [
      {
        id: "purpose",
        title: "1. Component Purpose",
        meta: "The role of the component inside a device",
        tags: ["purpose", "umec", "integration"],
        paragraphs: [
          "The component is meant to be embedded into an ESP32 device as a ready-made connectivity layer for UMEC Space IoT Cloud. Product firmware should not reimplement cloud onboarding, authorization, MQTT runtime, or OTA orchestration.",
          "It owns the repeatable integration work: local provisioning, storage of network and cloud parameters, device status publication, and inbound cloud command handling."
        ],
        bullets: [
          "Targets ESP-IDF and the ESP32 family.",
          "Separates integration logic from product-specific logic.",
          "External entry point: `core::init()`."
        ]
      },
      {
        id: "public-surface",
        title: "2. Public Surface",
        meta: "What the application passes in and what is observed from outside",
        tags: ["api", "entrypoint", "surface"],
        paragraphs: [
          "The public header lives in `components/core/include/core.h`. Through `CoreHardwareConfig`, the application supplies button and LED pin mapping and then hands connection lifecycle ownership to the component.",
          "Externally, the component is observed through the BLE provisioning contract, MQTT topics, LED indication, button behavior, persisted configuration, and the OTA path."
        ],
        bullets: [
          "Board mapping is supplied once during boot.",
          "Build identity is defined in `core_build_info.h`.",
          "Product code extends the device through producers and domain logic without touching transport internals."
        ]
      },
      {
        id: "architecture",
        title: "3. Architecture and Modules",
        meta: "Responsibility boundaries inside core",
        tags: ["architecture", "modules", "boundaries"],
        paragraphs: [
          "`core_app.cpp` owns lifecycle orchestration. `core_ble.cpp` owns provisioning transport. `core_mqtt.cpp` owns transport session and publish queue. `core_mqtt_process.cpp` handles routes. `core_http.cpp` serves remote config, time sync, and OTA. `core_storage.cpp` owns persisted state. `core_data_queue.cpp` owns the logical telemetry queue. `core_button_led.cpp` owns local UX.",
          "This split makes the component safer to extend: transport, persistence, local UX, and semantic command handling do not collapse into one mixed unit."
        ],
        bullets: [
          "Transport is separated from route-specific behavior.",
          "Persistence is separated from transient runtime flags.",
          "Local UX does not own networking."
        ]
      },
      {
        id: "runtime-state",
        title: "4. RuntimeState and Synchronization",
        meta: "Shared state across the component",
        tags: ["runtime", "state", "concurrency"],
        paragraphs: [
          "`RuntimeState` groups the component shared state: persisted config, transport queues, MQTT handle, connectivity flags, provisioning state, OTA guard, LED mode, and hardware ownership.",
          "Access to shared structures is split across mutexes and atomics so that configuration, transport queues, and connection flags remain predictable."
        ],
        bullets: [
          "`RuntimeConfig` keeps only long-lived integration parameters.",
          "Transient connection flags stay separate from persisted state.",
          "`ota_in_progress` and connection flags act as lifecycle guards."
        ]
      },
      {
        id: "provisioning",
        title: "5. Provisioning and Authorization",
        meta: "BLE bootstrap and device-code flow",
        tags: ["ble", "provisioning", "auth"],
        paragraphs: [
          "The provisioning path is built around BLE and supports Wi-Fi bootstrap, cloud endpoint delivery, and the device-code authorization flow.",
          "Sensitive work is not executed directly in BLE callback context: input first lands in a queue, then a regular task performs JSON parsing, HTTP requests, NVS writes, and reconnect side effects."
        ],
        bullets: [
          "`SERVER_DATA` stores external service endpoints.",
          "`GET_TOKEN` and `CHECK_AUTH` serve the user-code authorization flow.",
          "Provisioning reset is available through the button and the MQTT `reset` route."
        ]
      },
      {
        id: "mqtt-runtime",
        title: "6. Wi-Fi, MQTT, and Cloud Commands",
        meta: "Connection lifecycle and route model",
        tags: ["wifi", "mqtt", "routes"],
        paragraphs: [
          "After configuration restore, the device moves through Wi-Fi association, cloud authorization, and MQTT connect. When state changes, the component rebuilds the transport session in a controlled order instead of relying on implicit client state.",
          "The current baseline also escalates repeated MQTT failures: after 3 consecutive failures it forces token refresh, and after 5 consecutive failures it performs hard Wi-Fi/MQTT recovery with cooldown guards.",
          "The cloud-to-device contract is concentrated in the route table. At the moment it includes at least `upgrade`, `reboot`, `reset`, and `target-temp`.",
          "MQTT subscriptions are installed centrally from the same route model through `subscribe_core_mqtt_routes()`, so new commands should start from the route table instead of the transport callback."
        ],
        bullets: [
          "`core_mqtt.cpp` stays transport-only.",
          "`core_mqtt_process.cpp` owns semantic command handling.",
          "Status snapshots and device-data are published to separate MQTT endpoints."
        ]
      },
      {
        id: "telemetry",
        title: "7. Telemetry and dataQueue",
        meta: "How the component publishes data",
        tags: ["telemetry", "dataqueue", "thresholds"],
        paragraphs: [
          "`dataQueue` accepts semantic items rather than raw MQTT payloads. It builds a logical key, performs upsert, applies threshold policy, and only then passes data to the publish layer.",
          "This limits memory growth and makes it easier for product code to publish its own measurements and states."
        ],
        bullets: [
          "Telemetry producers do not depend on topic naming.",
          "Threshold rules are centralized in `core_thresholds.h` and are applied top-down by exact or prefix match.",
          "Both the logical queue and the MQTT queue are memory-bounded."
        ]
      },
      {
        id: "storage-maintenance",
        title: "8. Storage, Time, and OTA",
        meta: "Persistence model and maintenance loops",
        tags: ["storage", "time", "ota", "maintenance"],
        paragraphs: [
          "Persisted state is split by namespaces and stores only values that must survive reboot: Wi-Fi credentials, cloud endpoints, access tokens, device-code fields, and time recovery data.",
          "The maintenance task periodically performs time sync, remote config refresh, and token lifecycle upkeep. Token refresh now uses persisted `expires_in` metadata so access tokens can be refreshed proactively before expiry."
        ],
        bullets: [
          "Empty values clear NVS keys.",
          "Telemetry is gated until valid time is restored or synchronized.",
          "The OTA path switches boot partition only after a successful write session."
        ]
      },
      {
        id: "local-ux",
        title: "9. Button and LED Behavior",
        meta: "Locally observable device behavior",
        tags: ["button", "led", "local-ux"],
        paragraphs: [
          "The LEDs encode lifecycle phases such as provisioning, Wi-Fi connect, token wait, MQTT connect, online state, and error state. This makes local diagnostics possible even without external tools.",
          "The button acts as a recovery input. Destructive reset is committed on release after a sufficiently long hold, which lowers the chance of accidental reset."
        ],
        bullets: [
          "`ButtonHold` acts as explicit hold confirmation.",
          "The reset threshold is currently `>= 10000 ms`.",
          "LED behavior is derived from an explicit `LedMode` contract."
        ]
      },
      {
        id: "operations",
        title: "10. Operations and Diagnostics",
        meta: "What matters for integration and support",
        tags: ["operations", "diagnostics", "field"],
        paragraphs: [
          "For integration and field support, the most important observable surfaces are LED modes, MQTT status publication, device-data, route handling, remote config logs, and OTA outcomes.",
          "In operation, the device moves through repeatable phases: unprovisioned boot, Wi-Fi association, authorization, MQTT online, background maintenance, and when needed controlled recovery."
        ],
        bullets: [
          "Provisioning, authorization, and MQTT transport belong to the component itself.",
          "OTA and reset are privileged operational paths.",
          "Detailed reference tables and troubleshooting runbooks are available on separate pages."
        ],
        table: {
          headers: ["Property", "Observed Behavior", "Source"],
          rows: [
            ["MQTT transport queue", "Bounded to 64 messages", "core_mqtt.cpp"],
            ["Logical telemetry queue", "Upsert by method/device/parameter", "core_data_queue.cpp"],
            ["Button recovery threshold", "Reset on release after >= 10000 ms", "core_button_led.cpp"],
            ["MQTT recovery escalation", "3 failures -> token refresh, 5 failures -> hard Wi-Fi/MQTT reconnect", "core_mqtt.cpp"],
            ["Periodic maintenance cadence", "30 s loop; 24 h time sync and remote config refresh", "core_app.cpp"],
            ["OTA execution guard", "Only one OTA session is allowed at a time", "core_http.cpp / core_internal.h"]
          ]
        }
      }
    ]
  }
};

const state = {
  lang: localStorage.getItem("docs-lang") || "en"
};

const ids = {
  navList: document.getElementById("nav-list"),
  heroKicker: document.getElementById("hero-kicker"),
  heroTitle: document.getElementById("hero-title"),
  heroLead: document.getElementById("hero-lead"),
  heroNote: document.getElementById("hero-note"),
  volumeList: document.getElementById("volume-list"),
  sectionList: document.getElementById("section-list"),
  langRu: document.getElementById("lang-ru"),
  langEn: document.getElementById("lang-en")
};

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function renderTable(table) {
  if (!table || !table.headers || !table.rows) {
    return "";
  }

  return `
    <div class="doc-table-wrap">
      <table class="doc-table">
        <thead>
          <tr>${table.headers.map((header) => `<th>${escapeHtml(header)}</th>`).join("")}</tr>
        </thead>
        <tbody>
          ${table.rows
            .map((row) => `<tr>${row.map((cell) => `<td>${escapeHtml(cell)}</td>`).join("")}</tr>`)
            .join("")}
        </tbody>
      </table>
    </div>
  `;
}

function renderList(items, className) {
  if (!items || items.length === 0) {
    return "";
  }

  return `<ul class="${className}">${items.map((item) => `<li>${escapeHtml(item)}</li>`).join("")}</ul>`;
}

function renderVolumes(volumesTitle, volumesLead, volumes) {
  return `
    <section class="detail-links">
      <div class="section-card">
        <header>
          <div>
            <h3>${escapeHtml(volumesTitle)}</h3>
            <div class="section-meta">${escapeHtml(volumesLead)}</div>
          </div>
        </header>
        <div class="detail-grid">
          ${volumes
            .map(
              (item) => `
                <a class="detail-card" href="${escapeHtml(item.href)}">
                  <strong>${escapeHtml(item.title)}</strong>
                  <span>${escapeHtml(item.description)}</span>
                </a>
              `
            )
            .join("")}
        </div>
      </div>
    </section>
  `;
}

function render() {
  const t = content[state.lang];
  document.documentElement.lang = state.lang;
  document.title = "ESP-IDF Connectivity Component for UMEC Space IoT Cloud";

  ids.heroKicker.textContent = t.heroKicker;
  ids.heroTitle.textContent = t.heroTitle;
  ids.heroLead.textContent = t.heroLead;
  ids.heroNote.textContent = t.heroNote;
  ids.langRu.classList.toggle("active", state.lang === "ru");
  ids.langEn.classList.toggle("active", state.lang === "en");

  ids.navList.innerHTML = t.volumes
    .map((item) => `<li><a href="${escapeHtml(item.href)}">${escapeHtml(item.title)}</a></li>`)
    .join("");

  ids.volumeList.innerHTML = renderVolumes(t.volumesTitle, t.volumesLead, t.volumes);

  ids.sectionList.innerHTML = t.sections
    .map(
      (section) => `
        <article class="section-card" id="${escapeHtml(section.id)}">
          <header>
            <div>
              <h3>${escapeHtml(section.title)}</h3>
              <div class="section-meta">${escapeHtml(section.meta)}</div>
            </div>
          </header>
          ${section.paragraphs.map((paragraph) => `<p>${escapeHtml(paragraph)}</p>`).join("")}
          ${renderList(section.bullets, "")}
          ${renderTable(section.table)}
          <div class="tag-row">
            ${section.tags.map((tag) => `<span class="tag">${escapeHtml(tag)}</span>`).join("")}
          </div>
        </article>
      `
    )
    .join("");
}

ids.langRu.addEventListener("click", () => {
  state.lang = "ru";
  localStorage.setItem("docs-lang", state.lang);
  render();
});

ids.langEn.addEventListener("click", () => {
  state.lang = "en";
  localStorage.setItem("docs-lang", state.lang);
  render();
});

render();
