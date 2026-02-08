from flask import Flask, render_template, send_from_directory, abort, redirect, url_for, request, session, flash
from flask_wtf import FlaskForm
from wtforms import FileField, SubmitField
from werkzeug.utils import secure_filename
from werkzeug.security import generate_password_hash, check_password_hash
import pathlib
import os
import sqlite3
import uuid
import time
import shutil
from wtforms.validators import InputRequired

app = Flask(__name__)
app.config['SECRET_KEY'] = 'supersecretkey'
app.config['UPLOAD_FOLDER'] = 'storage/submissions'

BASE_DIR = os.path.abspath(os.path.dirname(__file__))
PUBLIC_DIR = os.path.join(BASE_DIR, 'storage', 'public')
SUBMISSIONS_DIR = os.path.join(BASE_DIR, 'storage', 'submissions')

ALLOWED_EXT = {'.leaf'}

def allowed_file(filename):
    return pathlib.Path(filename).suffix.lower() in ALLOWED_EXT

class UploadFileForm(FlaskForm):
    file = FileField("File", validators=[InputRequired()])
    submit = SubmitField("Upload File")

# --- simple sqlite-backed user store -----------------------------------------------------------------
DB_PATH = os.path.join(BASE_DIR, 'data', 'users.db')

# supported avatar extensions
AVATAR_EXTS = ['.jpg', '.jpeg', '.png']

def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        username TEXT UNIQUE NOT NULL,
        password_hash TEXT NOT NULL
    )''')
    # ensure optional columns exist (bio, username_changed_at)
    conn.commit()
    c.execute("PRAGMA table_info(users)")
    cols = [r[1] for r in c.fetchall()]
    if 'bio' not in cols:
        c.execute("ALTER TABLE users ADD COLUMN bio TEXT DEFAULT ''")
    if 'username_changed_at' not in cols:
        c.execute("ALTER TABLE users ADD COLUMN username_changed_at INTEGER DEFAULT 0")
    # Add uuid column without UNIQUE (SQLite can't add UNIQUE via ALTER), then backfill and index
    if 'uuid' not in cols:
        c.execute("ALTER TABLE users ADD COLUMN uuid TEXT")
        conn.commit()
        # backfill missing uuids
        c.execute("SELECT id FROM users WHERE uuid IS NULL OR uuid = ''")
        rows = c.fetchall()
        for (uid,) in rows:
            c.execute("UPDATE users SET uuid = ? WHERE id = ?", (str(uuid.uuid4()), uid))
        conn.commit()
        # ensure unique index exists
        c.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_users_uuid ON users(uuid)")
    # add role column if missing
    if 'role' not in cols:
        c.execute("ALTER TABLE users ADD COLUMN role TEXT DEFAULT 'member'")
    # ensure specific owner user
    try:
        c.execute("UPDATE users SET role = 'owner' WHERE LOWER(username) = 'frogman'")
    except Exception:
        pass
    conn.commit()
    conn.close()

def get_user_by_username(username):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('SELECT id, username, password_hash, bio, username_changed_at, uuid, role FROM users WHERE username = ?', (username,))
    row = c.fetchone()
    conn.close()
    if not row:
        return None
    return {
        'id': row[0],
        'username': row[1],
        'password_hash': row[2],
        'bio': row[3] or '',
        'username_changed_at': int(row[4] or 0),
        'uuid': row[5] or '',
        'role': (row[6] or 'member').lower()
    }

def get_user_by_uuid(user_uuid):
    if not user_uuid:
        return None
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('SELECT id, username, password_hash, bio, username_changed_at, uuid, role FROM users WHERE uuid = ?', (user_uuid,))
    row = c.fetchone()
    conn.close()
    if not row:
        return None
    return {
        'id': row[0],
        'username': row[1],
        'password_hash': row[2],
        'bio': row[3] or '',
        'username_changed_at': int(row[4] or 0),
        'uuid': row[5] or '',
        'role': (row[6] or 'member').lower()
    }

def create_user(username, password):
    try:
        conn = sqlite3.connect(DB_PATH)
        c = conn.cursor()
        ph = generate_password_hash(password)
        user_uuid = str(uuid.uuid4())
        role = 'owner' if username.lower() == 'frogman' else 'member'
        c.execute('INSERT INTO users (username, password_hash, bio, username_changed_at, uuid, role) VALUES (?, ?, ?, ?, ?, ?)', (username, ph, '', 0, user_uuid, role))
        conn.commit()
        conn.close()
        return True
    except Exception:
        return False

init_db()
# -----------------------------------------------------------------------------------------------------

@app.before_request
def sync_session_from_db():
    # keep session role/uuid in sync with DB so promotions take effect immediately
    suuid = session.get('uuid')
    u = None
    if suuid:
        u = get_user_by_uuid(suuid)
    else:
        uname = session.get('user')
        if uname:
            u = get_user_by_username(uname)
    if u:
        # Always reflect latest DB state in session
        session['role'] = u.get('role', 'member')
        session['user'] = u.get('username')
        if u.get('uuid'):
            session['uuid'] = u.get('uuid')


@app.context_processor
def inject_nav_context():
    # Provide current user's avatar URL to templates
    avatar_url = None
    uname = session.get('user')
    if uname:
        # default avatar
        profile_url = url_for('static', filename='images/defaultprofilepicture.jpg')
        profiles_dir = os.path.join(BASE_DIR, 'static', 'images', 'profiles')
        for ext in AVATAR_EXTS:
            candidate = os.path.join(profiles_dir, f"{uname}{ext}")
            if os.path.isfile(candidate):
                try:
                    mtime = int(os.path.getmtime(candidate))
                except Exception:
                    mtime = 0
                profile_url = url_for('static', filename=f'images/profiles/{uname}{ext}', v=mtime)
                break
        avatar_url = profile_url
    return {'current_avatar_url': avatar_url}

@app.route('/', methods=['GET'])
def index():
    return render_template('index.html')


@app.route('/home')
def home_redirect():
    return render_template('index.html')


@app.route('/upload', methods=['GET', 'POST'])
def upload():
    # require login
    if not session.get('user'):
        return redirect(url_for('login', next=url_for('upload')))

    form = UploadFileForm()
    if form.validate_on_submit():
        file = form.file.data
        filename = secure_filename(file.filename)
        if not allowed_file(filename):
            abort(400, 'Only .leaf files are allowed')
        role = (session.get('role') or 'member').lower()
        if role in ('admin', 'owner'):
            # privileged users: upload directly to public under user's namespace
            user = session.get('user')
            user_public = os.path.join(PUBLIC_DIR, user)
            os.makedirs(user_public, exist_ok=True)
            dest = os.path.join(user_public, filename)
            file.save(dest)
        else:
            # store submissions under a per-user directory
            user = session.get('user')
            user_dir = os.path.join(SUBMISSIONS_DIR, user)
            dest = os.path.join(user_dir, filename)
            os.makedirs(user_dir, exist_ok=True)
            file.save(dest)
        return redirect(url_for('upload_success'))
    return render_template('upload.html', form=form)


@app.route('/signup', methods=['GET', 'POST'])
def signup():
    if request.method == 'POST':
        username = request.form.get('username', '').strip()
        password = request.form.get('password', '')
        if not username or not password:
            flash('Username and password are required')
            return render_template('signup.html')
        ok = create_user(username, password)
        if not ok:
            flash('Could not create user — username may already exist')
            return render_template('signup.html')
        # populate session with new user info
        u = get_user_by_username(username)
        session['user'] = username
        session['user_id'] = u['id'] if u else None
        session['uuid'] = u['uuid'] if u else None
        session['role'] = (u['role'] if u else 'member')
        return redirect(url_for('index'))
    return render_template('signup.html')


@app.route('/login', methods=['GET', 'POST'])
def login():
    # Clear any stale flash messages so login page is clean
    session.pop('_flashes', None)
    if request.method == 'POST':
        username = request.form.get('username', '').strip()
        password = request.form.get('password', '')
        if not username or not password:
            flash('Username and password required')
            return render_template('login.html')
        user = get_user_by_username(username)
        if not user or not check_password_hash(user['password_hash'], password):
            flash('Invalid credentials')
            return render_template('login.html')
        session['user'] = user['username']
        session['user_id'] = user['id']
        session['uuid'] = user.get('uuid')
        session['role'] = user.get('role', 'member')
        nxt = request.args.get('next') or url_for('index')
        return redirect(nxt)
    return render_template('login.html')


@app.route('/logout')
def logout():
    session.pop('user', None)
    session.pop('user_id', None)
    session.pop('uuid', None)
    session.pop('role', None)
    # Clear any queued flash messages so next page doesn't show old notices
    session.pop('_flashes', None)
    return redirect(url_for('index'))


@app.route('/upload/success')
def upload_success():
    return render_template('uploadSuccess.html')


@app.route('/manifest/sample.leaf')
def manifest_sample():
    return redirect(url_for('example'))


@app.route('/manifest/<path:filename>')
def manifest(filename):
    if not allowed_file(filename):
        abort(400)
    requested_path = os.path.normpath(os.path.join(PUBLIC_DIR, filename))
    try:
        common = os.path.commonpath([PUBLIC_DIR, requested_path])
    except ValueError:
        abort(400)
    if common != PUBLIC_DIR:
        abort(403)
    if not os.path.isfile(requested_path):
        abort(404)
    relpath = os.path.relpath(requested_path, PUBLIC_DIR)
    return send_from_directory(PUBLIC_DIR, relpath, as_attachment=True)


@app.route('/users/<username>')
def user_profile(username):
    # require that the username exists in the user DB
    user = get_user_by_username(username)
    if not user:
        abort(404)

    # find a profile image with a supported extension, fall back to default
    profile_url = url_for('static', filename='images/defaultprofilepicture.jpg')
    profiles_dir = os.path.join(BASE_DIR, 'static', 'images', 'profiles')
    for ext in AVATAR_EXTS:
        candidate = os.path.join(profiles_dir, f"{username}{ext}")
        if os.path.isfile(candidate):
            try:
                mtime = int(os.path.getmtime(candidate))
            except Exception:
                mtime = 0
            profile_url = url_for('static', filename=f'images/profiles/{username}{ext}', v=mtime)
            break

    # list packages only from public area
    user_dir = os.path.join(PUBLIC_DIR, username)
    packages = []
    if os.path.isdir(user_dir):
        for fn in sorted(os.listdir(user_dir)):
            if allowed_file(fn):
                packages.append(fn)

    bio = user.get('bio', '') if user else ''
    role = (user.get('role', 'member') if user else 'member')
    return render_template('account.html', username=username, profile_url=profile_url, packages=packages, bio=bio, role=role)


@app.route('/userfiles/<username>/<path:filename>')
def user_file_download(username, filename):
    # serve a user's public file; only allow .leaf and safe paths
    if not allowed_file(filename):
        abort(400)
    # ensure the user exists
    if not get_user_by_username(username):
        abort(404)
    user_dir = os.path.join(PUBLIC_DIR, username)
    requested_path = os.path.normpath(os.path.join(user_dir, filename))
    try:
        common = os.path.commonpath([user_dir, requested_path])
    except ValueError:
        abort(400)
    if common != user_dir:
        abort(403)
    if not os.path.isfile(requested_path):
        abort(404)
    relpath = os.path.relpath(requested_path, user_dir)
    return send_from_directory(user_dir, relpath, as_attachment=True)


# ------------------------ Admin Review ------------------------
@app.route('/admin/review')
def admin_review():
    role = (session.get('role') or 'member').lower()
    if role not in ('admin', 'owner'):
        abort(403)
    # collect all pending submissions by user
    pending = []
    if os.path.isdir(SUBMISSIONS_DIR):
        for uname in sorted(os.listdir(SUBMISSIONS_DIR)):
            udir = os.path.join(SUBMISSIONS_DIR, uname)
            if os.path.isdir(udir):
                for fn in sorted(os.listdir(udir)):
                    if allowed_file(fn):
                        pending.append({'username': uname, 'filename': fn})
    # optionally view a selected file
    sel_user = request.args.get('u', '').strip()
    sel_file = request.args.get('f', '').strip()
    sel_content = None
    if sel_user and sel_file and allowed_file(sel_file):
        view_path = os.path.normpath(os.path.join(SUBMISSIONS_DIR, sel_user, sel_file))
        try:
            common = os.path.commonpath([os.path.join(SUBMISSIONS_DIR, sel_user), view_path])
        except ValueError:
            view_path = None
        if view_path and os.path.isfile(view_path):
            try:
                with open(view_path, 'r', encoding='utf-8') as f:
                    sel_content = f.read()
            except Exception:
                sel_content = '(unable to read file)'
    return render_template('admin_review.html', pending=pending, sel_user=sel_user, sel_file=sel_file, sel_content=sel_content)


@app.route('/admin/review/accept', methods=['POST'])
def admin_accept():
    role = (session.get('role') or 'member').lower()
    if role not in ('admin', 'owner'):
        abort(403)
    username = request.form.get('username', '').strip()
    filename = request.form.get('filename', '').strip()
    if not username or not allowed_file(filename):
        abort(400)
    src_dir = os.path.join(SUBMISSIONS_DIR, username)
    src = os.path.normpath(os.path.join(src_dir, filename))
    try:
        common = os.path.commonpath([src_dir, src])
    except ValueError:
        abort(400)
    if common != src_dir or not os.path.isfile(src):
        abort(404)
    # move to public under user's namespace
    dst_dir = os.path.join(PUBLIC_DIR, username)
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, filename)
    try:
        shutil.move(src, dst)
        flash('Accepted and published')
    except Exception:
        flash('Failed to publish')
    return redirect(url_for('admin_review', u=username, f=filename))


@app.route('/admin/review/deny', methods=['POST'])
def admin_deny():
    role = (session.get('role') or 'member').lower()
    if role not in ('admin', 'owner'):
        abort(403)
    username = request.form.get('username', '').strip()
    filename = request.form.get('filename', '').strip()
    if not username or not allowed_file(filename):
        abort(400)
    src_dir = os.path.join(SUBMISSIONS_DIR, username)
    src = os.path.normpath(os.path.join(src_dir, filename))
    try:
        common = os.path.commonpath([src_dir, src])
    except ValueError:
        abort(400)
    if common != src_dir:
        abort(403)
    if os.path.isfile(src):
        try:
            os.remove(src)
            flash('Denied and removed')
        except Exception:
            flash('Failed to remove')
    else:
        flash('Package not found')
    return redirect(url_for('admin_review'))


@app.route('/users/<username>/delete/<path:filename>', methods=['POST'])
def delete_user_file(username, filename):
    # only allow deletion of .leaf files within the user's submissions directory
    if not allowed_file(filename):
        abort(400)
    actor = session.get('user')
    actor_role = (session.get('role') or 'member').lower()
    if not actor:
        abort(403)
    # only admin/owner may delete packages (any user's)
    if actor_role not in ('admin', 'owner'):
        abort(403)
    user_dir = os.path.join(SUBMISSIONS_DIR, username)
    requested_path = os.path.normpath(os.path.join(user_dir, filename))
    try:
        common = os.path.commonpath([user_dir, requested_path])
    except ValueError:
        abort(400)
    if common != user_dir:
        abort(403)
    if os.path.isfile(requested_path):
        try:
            os.remove(requested_path)
            flash('Package deleted')
        except Exception:
            flash('Failed to delete package')
    else:
        flash('Package not found')
    return redirect(url_for('user_profile', username=username))


@app.route('/users/<username>/edit', methods=['POST'])
def edit_profile(username):
    # only allow the profile owner to edit
    if not session.get('user') or session.get('user') != username:
        abort(403)

    # handle bio update and optional username change
    new_username = request.form.get('new_username', '').strip()
    new_bio = request.form.get('bio', '').strip()

    # reload user info from DB
    u = get_user_by_username(username)
    if not u:
        abort(404)

    # change username if requested
    if new_username and new_username != username:
        # enforce 7 day cooldown unless admin/owner
        now = int(time.time())
        last = int(u.get('username_changed_at') or 0)
        role = (session.get('role') or u.get('role') or 'member').lower()
        if role not in ('admin', 'owner'):
            if last != 0 and (now - last) < 7 * 24 * 3600:
                flash('You may change your username only once every 7 days')
                return redirect(url_for('user_profile', username=username))
        # ensure new username not taken
        if get_user_by_username(new_username):
            flash('Username already taken')
            return redirect(url_for('user_profile', username=username))

        # update DB
        try:
            conn = sqlite3.connect(DB_PATH)
            c = conn.cursor()
            c.execute('UPDATE users SET username = ?, username_changed_at = ? WHERE id = ?', (new_username, now, u['id']))
            conn.commit()
            conn.close()
        except Exception:
            flash('Failed to change username')
            return redirect(url_for('user_profile', username=username))

        # move user's submission directory if exists
        old_dir = os.path.join(SUBMISSIONS_DIR, username)
        new_dir = os.path.join(SUBMISSIONS_DIR, new_username)
        try:
            if os.path.isdir(old_dir):
                shutil.move(old_dir, new_dir)
        except Exception:
            pass

        # move avatar files
        profiles_dir = os.path.join(BASE_DIR, 'static', 'images', 'profiles')
        for e in AVATAR_EXTS:
            oldp = os.path.join(profiles_dir, f"{username}{e}")
            newp = os.path.join(profiles_dir, f"{new_username}{e}")
            try:
                if os.path.isfile(oldp):
                    shutil.move(oldp, newp)
            except Exception:
                pass

        # update session username
        session['user'] = new_username
        username = new_username

    # update bio
    try:
        conn = sqlite3.connect(DB_PATH)
        c = conn.cursor()
        c.execute('UPDATE users SET bio = ? WHERE username = ?', (new_bio, username))
        conn.commit()
        conn.close()
    except Exception:
        flash('Failed to save bio')
        return redirect(url_for('user_profile', username=username))

    # handle avatar upload if present
    if 'avatar' in request.files:
        f = request.files['avatar']
        if f and f.filename:
            filename = secure_filename(f.filename)
            ext = os.path.splitext(filename)[1].lower()
            if ext in AVATAR_EXTS:
                profiles_dir = os.path.join(BASE_DIR, 'static', 'images', 'profiles')
                os.makedirs(profiles_dir, exist_ok=True)
                # remove existing avatar files with other extensions
                for e in AVATAR_EXTS:
                    existing = os.path.join(profiles_dir, f"{username}{e}")
                    try:
                        if os.path.isfile(existing):
                            os.remove(existing)
                    except Exception:
                        pass
                dest = os.path.join(profiles_dir, f"{username}{ext}")
                f.save(dest)

    flash('Profile updated')
    return redirect(url_for('user_profile', username=username))




@app.errorhandler(404)
def page_not_found(e):
    return render_template('404.html'), 404


@app.route('/example')
def example():
    # Attempt to read the example file from storage/example/example.leaf
    example_path = os.path.join(BASE_DIR, 'storage', 'example', 'example.leaf')
    sample = None
    try:
        with open(example_path, 'r', encoding='utf-8') as f:
            sample = f.read()
    except Exception:
        # fallback sample if file missing or unreadable
        sample = '''# Example .leaf manifest
name: Example Project
version: 1.0.0
description: A short placeholder description for this example manifest.
files:
  - path: hello.txt
    sha256: 3b8f1e...placeholder
  - path: data/config.json
    sha256: a7d4c2...placeholder
'''

    return render_template('example_leaf.html', code=sample)


@app.route('/example/download')
def example_download():
    # Serve the example file directly from storage/example (single allowed file)
    example_dir = os.path.join(BASE_DIR, 'storage', 'example')
    filename = 'example.leaf'
    requested = os.path.join(example_dir, filename)
    if not os.path.isfile(requested):
        abort(404)
    return send_from_directory(example_dir, filename, as_attachment=True)

if __name__ == '__main__':
    app.run(debug=True)