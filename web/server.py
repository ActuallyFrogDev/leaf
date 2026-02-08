from flask import Flask, render_template, send_from_directory, abort, redirect, url_for, request, session, flash
from flask_wtf import FlaskForm
from wtforms import FileField, SubmitField
from werkzeug.utils import secure_filename
from werkzeug.security import generate_password_hash, check_password_hash
import pathlib
import os
import sqlite3
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

def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        username TEXT UNIQUE NOT NULL,
        password_hash TEXT NOT NULL
    )''')
    conn.commit()
    conn.close()

def get_user_by_username(username):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('SELECT id, username, password_hash FROM users WHERE username = ?', (username,))
    row = c.fetchone()
    conn.close()
    if not row:
        return None
    return {'id': row[0], 'username': row[1], 'password_hash': row[2]}

def create_user(username, password):
    try:
        conn = sqlite3.connect(DB_PATH)
        c = conn.cursor()
        ph = generate_password_hash(password)
        c.execute('INSERT INTO users (username, password_hash) VALUES (?, ?)', (username, ph))
        conn.commit()
        conn.close()
        return True
    except Exception:
        return False

init_db()
# -----------------------------------------------------------------------------------------------------

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
        session['user'] = username
        flash('Account created and logged in')
        return redirect(url_for('index'))
    return render_template('signup.html')


@app.route('/login', methods=['GET', 'POST'])
def login():
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
        nxt = request.args.get('next') or url_for('index')
        return redirect(nxt)
    return render_template('login.html')


@app.route('/logout')
def logout():
    session.pop('user', None)
    session.pop('user_id', None)
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

    profile_static = os.path.join(BASE_DIR, 'static', 'images', 'profiles', f"{username}.jpg")
    if os.path.isfile(profile_static):
        profile_url = url_for('static', filename=f'images/profiles/{username}.jpg')
    else:
        profile_url = url_for('static', filename='images/defaultprofilepicture.jpg')

    user_dir = os.path.join(SUBMISSIONS_DIR, username)
    packages = []
    if os.path.isdir(user_dir):
        for fn in sorted(os.listdir(user_dir)):
            if allowed_file(fn):
                packages.append(fn)

    return render_template('account.html', username=username, profile_url=profile_url, packages=packages)


@app.route('/userfiles/<username>/<path:filename>')
def user_file_download(username, filename):
    # serve a user's submitted file; only allow .leaf and safe paths
    if not allowed_file(filename):
        abort(400)
    # ensure the user exists
    if not get_user_by_username(username):
        abort(404)
    user_dir = os.path.join(SUBMISSIONS_DIR, username)
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