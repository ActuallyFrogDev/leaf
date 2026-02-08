from flask import Flask, render_template, send_from_directory, abort, redirect, url_for
from flask_wtf import FlaskForm
from wtforms import FileField, SubmitField
from werkzeug.utils import secure_filename
import pathlib
import os
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

@app.route('/', methods=['GET'])
def index():
    return render_template('index.html')


@app.route('/home')
def home_redirect():
    return render_template('index.html')


@app.route('/upload', methods=['GET', 'POST'])
def upload():
    form = UploadFileForm()
    if form.validate_on_submit():
        file = form.file.data
        filename = secure_filename(file.filename)
        if not allowed_file(filename):
            abort(400, 'Only .leaf files are allowed')
        dest = os.path.join(SUBMISSIONS_DIR, filename)
        os.makedirs(SUBMISSIONS_DIR, exist_ok=True)
        file.save(dest)
        # Use Post/Redirect/Get to avoid form re-submission and ensure
        # the success page is rendered via a GET request.
        return redirect(url_for('upload_success'))
    return render_template('upload.html', form=form)


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