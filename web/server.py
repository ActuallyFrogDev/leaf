from flask import Flask, render_template, send_from_directory, abort
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
        return render_template('uploadSuccess.html')
    return render_template('upload.html', form=form)


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

if __name__ == '__main__':
    app.run(debug=True)