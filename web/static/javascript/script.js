document.addEventListener("DOMContentLoaded", function () {
  // show selected filename for custom file input
  document.querySelectorAll(".input-file").forEach(function (inp) {
    inp.addEventListener("change", function (e) {
      const file = inp.files && inp.files[0];
      const label =
        inp.closest(".file-input") &&
        inp.closest(".file-input").querySelector(".file-label");
      if (file && label) {
        label.textContent = file.name;
        // mark the drop zone as ready
        const dz = inp.closest(".drop-zone");
        if (dz) dz.classList.add("ready");
      }
    });
  });

  // drag-and-drop handling for .drop-zone
  document.querySelectorAll(".drop-zone").forEach(function (dz) {
    const input = dz.querySelector(".input-file");
    // click anywhere on zone to open file picker
    dz.addEventListener("click", function (e) {
      // only respond to primary button
      if (e.button && e.button !== 0) return;
      // if the user clicked the native input element itself, let it handle the picker
      if (e.target === input || e.target.closest(".input-file")) return;
      // otherwise prevent default and open the picker programmatically once
      e.preventDefault();
      e.stopPropagation();
      if (input) input.click();
    });

    function prevent(e) {
      e.preventDefault();
      e.stopPropagation();
    }

    dz.addEventListener("dragenter", function (e) {
      prevent(e);
      dz.classList.add("dragover");
    });
    dz.addEventListener("dragover", function (e) {
      prevent(e);
      dz.classList.add("dragover");
    });
    dz.addEventListener("dragleave", function (e) {
      prevent(e);
      dz.classList.remove("dragover");
    });
    dz.addEventListener("drop", function (e) {
      prevent(e);
      dz.classList.remove("dragover");
      const files = (e.dataTransfer && e.dataTransfer.files) || [];
      if (!files.length) return;
      const f = files[0];
      // only accept .leaf files
      if (!f.name || !f.name.toLowerCase().endsWith(".leaf")) {
        const hint = dz.querySelector(".drop-hint");
        if (hint) {
          hint.textContent = "Only .leaf files allowed";
        }
        dz.classList.remove("ready");
        return;
      }
      // assign file to input (some browsers allow DataTransfer)
      try {
        const dataTransfer = new DataTransfer();
        dataTransfer.items.add(f);
        if (input) input.files = dataTransfer.files;
      } catch (err) {
        // fallback: can't programmatically set FileList in some browsers
      }
      // update label and mark ready
      const label = dz.querySelector(".file-label");
      if (label) label.textContent = f.name;
      dz.classList.add("ready");
    });
  });

  // small visual flourish: add loaded class after paint
  requestAnimationFrame(() =>
    document
      .querySelectorAll(".fade-in")
      .forEach((el) => el.classList.add("loaded")),
  );
  // Add line numbers to IDE-like code blocks
  function attachLineNumbers() {
    document.querySelectorAll("#leaf-code").forEach(function (codeEl) {
      const txt = codeEl.textContent || "";
      const lines = txt.split("\n");
      const container = codeEl.closest(".ide");
      if (!container) return;
      const ln = container.querySelector(".line-numbers");
      if (!ln) return;
      // compose numbers
      ln.innerHTML = "";
      for (let i = 1; i <= lines.length; i++) {
        const div = document.createElement("div");
        div.textContent = i;
        ln.appendChild(div);
      }
    });
  }
  attachLineNumbers();
  // keep numbers in sync if window resizes (wraps may change lines visually)
  let resizeTimer = null;
  window.addEventListener("resize", function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(attachLineNumbers, 250);
  });
  // Button ripple effect
  document.querySelectorAll(".btn").forEach(function (btn) {
    btn.addEventListener("click", function (e) {
      const rect = btn.getBoundingClientRect();
      const ripple = document.createElement("span");
      ripple.className = "ripple";
      const size = Math.max(rect.width, rect.height) * 1.2;
      ripple.style.width = ripple.style.height = size + "px";
      ripple.style.left = e.clientX - rect.left - size / 2 + "px";
      ripple.style.top = e.clientY - rect.top - size / 2 + "px";
      btn.appendChild(ripple);
      setTimeout(() => {
        try {
          btn.removeChild(ripple);
        } catch (_) {}
      }, 700);
    });
  });

  // Trigger arrow animation on upload success page
  document.querySelectorAll(".arrow").forEach(function (svg) {
    // only animate once, after a short delay so page paint completes
    setTimeout(function () {
      svg.classList.add("animated");
    }, 140);
  });

  // Reveal nav links with a stagger
  const navLinks = document.querySelectorAll(".nav a");
  if (navLinks && navLinks.length) {
    navLinks.forEach(function (a, i) {
      setTimeout(
        function () {
          a.classList.add("nav-in");
        },
        120 + i * 80,
      );
    });
  }

  // Enhanced upload handling: XHR with progress and auto-start on drop
  const uploadForm = document.getElementById("upload-form");
  const previewName = document.getElementById("preview-name");
  const previewBox = document.getElementById("upload-preview");
  const progressBar = document.getElementById("upload-progress-bar");
  const submitBtn = document.getElementById("submit-btn");

  function showPreview(name) {
    if (previewName) previewName.textContent = name;
    if (previewBox) previewBox.classList.add("visible");
  }

  function setProgress(pct) {
    if (!progressBar) return;
    const clamped = Math.min(100, Math.max(0, Math.round(pct * 100)));
    progressBar.style.width = clamped + "%";
  }

  async function startUpload(form) {
    if (!form) return;
    // disable submit while uploading
    if (submitBtn) submitBtn.disabled = true;
    const fd = new FormData(form);
    const xhr = new XMLHttpRequest();
    xhr.open("POST", form.action || window.location.href);
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable) setProgress(e.loaded / e.total);
    };
    xhr.onload = function () {
      // on success, redirect to upload success page (server-side redirect may not change location)
      if (xhr.status >= 200 && xhr.status < 400) {
        window.location = "/upload/success";
      } else {
        if (submitBtn) submitBtn.disabled = false;
        alert("Upload failed. Please try again.");
      }
    };
    xhr.onerror = function () {
      if (submitBtn) submitBtn.disabled = false;
      alert("Upload failed due to network error.");
    };
    xhr.send(fd);
  }

  if (uploadForm) {
    uploadForm.addEventListener("submit", function (e) {
      e.preventDefault();
      const input = uploadForm.querySelector(".input-file");
      const f = input && input.files && input.files[0];
      if (!f) {
        alert("Please select a .leaf file to upload.");
        return;
      }
      startUpload(uploadForm);
    });
  }

  // auto-start upload on drop when file present
  document.querySelectorAll(".drop-zone").forEach(function (dz) {
    dz.addEventListener("drop", function (e) {
      setTimeout(function () {
        const input = dz.querySelector(".input-file");
        const f = input && input.files && input.files[0];
        if (f) {
          showPreview(f.name);
          // start upload automatically
          startUpload(uploadForm);
        }
      }, 80);
    });
    // when input changes via file picker, show preview
    const inp = dz.querySelector(".input-file");
    if (inp) {
      inp.addEventListener("change", function () {
        const f = inp.files && inp.files[0];
        if (f) showPreview(f.name);
      });
    }
  });
});
