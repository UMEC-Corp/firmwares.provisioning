(function () {
  if (!window.mermaid) {
    return;
  }

  window.mermaid.initialize({
    startOnLoad: false,
    theme: "base",
    securityLevel: "loose",
    flowchart: {
      useMaxWidth: true,
      htmlLabels: true,
      curve: "basis"
    },
    sequence: {
      useMaxWidth: true,
      wrap: true
    },
    state: {
      useMaxWidth: true
    },
    themeVariables: {
      primaryColor: "#dcecef",
      primaryBorderColor: "#0e5a61",
      primaryTextColor: "#1e2430",
      secondaryColor: "#f5e1d8",
      secondaryBorderColor: "#b54f2d",
      secondaryTextColor: "#1e2430",
      tertiaryColor: "#f4efe6",
      tertiaryBorderColor: "#5f6b7a",
      tertiaryTextColor: "#1e2430",
      lineColor: "#5f6b7a",
      noteBkgColor: "#fff7df",
      noteBorderColor: "#d1a437",
      actorBorder: "#0e5a61",
      actorBkg: "#dcecef",
      actorTextColor: "#1e2430",
      activationBorderColor: "#b54f2d",
      activationBkgColor: "#f5e1d8"
    }
  });

  window.addEventListener("load", function () {
    window.mermaid.run({
      querySelector: ".mermaid"
    });
  });
}());
