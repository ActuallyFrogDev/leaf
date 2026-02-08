document.addEventListener("DOMContentLoaded", function () {
  // Cookie helpers to remember username for UX
  function setCookie(name, value, days) {
    let expires = "";
    if (days) {
      const d = new Date();
      d.setTime(d.getTime() + days * 24 * 60 * 60 * 1000);
      expires = "; expires=" + d.toUTCString();
    }
    document.cookie =
      name + "=" + encodeURIComponent(value) + expires + "; path=/";
  }

  function getCookie(name) {
    const v = document.cookie.match("(?:^|; )" + name + "=([^;]*)");
    return v ? decodeURIComponent(v[1]) : null;
  }

  // Prefill username fields from cookie if available
  const saved = getCookie("leaf_username");
  if (saved) {
    document.querySelectorAll('input[name="username"]').forEach(function (i) {
      if (!i.value) i.value = saved;
    });
  }

  // detect whether user is logged in by presence of a Logout link or greeting
  function queryContains(selector, text) {
    const nodes = document.querySelectorAll(selector);
    for (let n of nodes)
      if ((n.textContent || "").trim().includes(text)) return n;
    return null;
  }

  const logout =
    document.querySelector('.nav a[href*="logout"]') ||
    queryContains(".nav a", "Logout");
  const hi = queryContains(".nav a", "Hi,");
  const logged = !!logout || !!hi;

  // If not logged, make upload CTA redirect to login early for better UX
  if (!logged) {
    const ctas = document.querySelectorAll(
      'a.btn.primary[href="/upload"], a.nav-link.primary-nav[href="/upload"]',
    );
    ctas.forEach(function (a) {
      a.setAttribute("href", "/login?next=/upload");
      a.setAttribute("title", "Sign in to upload");
      a.classList.add("requires-auth");
    });

    // On upload page, prevent upload actions and redirect to login
    const uploadForm = document.getElementById("upload-form");
    if (uploadForm) {
      uploadForm.addEventListener("submit", function (e) {
        e.preventDefault();
        window.location = "/login?next=/upload";
      });
      document.querySelectorAll(".drop-zone").forEach(function (dz) {
        dz.addEventListener("drop", function (e) {
          e.preventDefault();
          e.stopPropagation();
          window.location = "/login?next=/upload";
        });
      });
    }
  }

  // Login/Signup page UX improvements
  const loginForm = document.querySelector('form[action="/login"]');
  const signupForm = document.querySelector('form[action="/signup"]');

  function attachFormUX(form, opts) {
    if (!form) return;
    let msg = document.createElement("div");
    msg.className = "client-note muted";
    form.parentNode.insertBefore(msg, form);

    form.addEventListener("submit", function (e) {
      const username =
        (form.querySelector('input[name="username"]') || {}).value || "";
      const password =
        (form.querySelector('input[name="password"]') || {}).value || "";
      msg.textContent = "";
      if (!username.trim()) {
        e.preventDefault();
        msg.textContent = "Username is required";
        return;
      }
      if (!password) {
        e.preventDefault();
        msg.textContent = "Password is required";
        return;
      }
      if (opts && opts.minPassword && password.length < opts.minPassword) {
        e.preventDefault();
        msg.textContent =
          "Password must be at least " + opts.minPassword + " characters";
        return;
      }
      // store username in cookie for future visits
      try {
        setCookie("leaf_username", username.trim(), 365);
      } catch (err) {
        /* ignore cookie failures */
      }
    });

    const pwd = form.querySelector('input[type="password"]');
    if (pwd) {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btn ghost small toggle-pw";
      btn.style.marginLeft = "8px";
      btn.textContent = "Show";
      btn.addEventListener("click", function (e) {
        e.preventDefault();
        if (pwd.type === "password") {
          pwd.type = "text";
          btn.textContent = "Hide";
        } else {
          pwd.type = "password";
          btn.textContent = "Show";
        }
      });
      pwd.parentNode.appendChild(btn);
    }
  }

  attachFormUX(loginForm, { minPassword: 1 });
  attachFormUX(signupForm, { minPassword: 6 });

  // small nav micro-interaction: highlight auth-required buttons
  document.querySelectorAll(".requires-auth").forEach(function (el) {
    el.addEventListener("mouseenter", function () {
      el.classList.add("auth-hover");
    });
    el.addEventListener("mouseleave", function () {
      el.classList.remove("auth-hover");
    });
  });
});
