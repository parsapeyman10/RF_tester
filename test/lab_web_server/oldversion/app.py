# Import necessary modules from Flask and Flask-SQLAlchemy
from flask import Flask, render_template, request, redirect, url_for, flash, jsonify, Response
from flask_sqlalchemy import SQLAlchemy
import datetime # To get current current time and date if not provided by ESP32
import io
import csv # For CSV export

# Create a Flask web application instance
app = Flask(__name__)
# Add a secret key for flash messages (optional but good practice)
app.config['SECRET_KEY'] = 'your_secret_key_here' # IMPORTANT: Change this to a strong, random key in production

# --- Database Configuration ---
# Configure the SQLite database URI
# 'sqlite:///site.db' means a file named 'site.db' will be created
# in the same directory as app.py
app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:///site.db'
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False # Disable tracking modifications overhead

# Initialize the SQLAlchemy instance with the Flask app
db = SQLAlchemy(app)

# --- Database Model Definition ---
# This class defines the structure of your database table
class SensorReading(db.Model):
    id = db.Column(db.Integer, primary_key=True) # Unique ID for each entry
    nbcm_selected = db.Column(db.String(100), nullable=True) # Comma-separated string of selected NBCMs
    humidity = db.Column(db.String(50), nullable=True)
    temp = db.Column(db.String(50), nullable=True)
    time = db.Column(db.String(50), nullable=True) # Storing as string for simplicity
    date = db.Column(db.String(50), nullable=True) # Storing as string for simplicity
    # Added a timestamp for when the data was received by the server
    timestamp = db.Column(db.DateTime, default=datetime.datetime.utcnow)

    # NEW COLUMN: To store the entire formatted log string as requested
    formatted_log = db.Column(db.String(500), nullable=True) # Increased length for the full string

    # Optional: A __repr__ method for better debugging
    def __repr__(self):
        return f"SensorReading(ID: {self.id}, Formatted Log: '{self.formatted_log}')"

# --- Flask Routes ---

# Route for the home page (where the HTML form is displayed)
@app.route('/')
def home():
    # Fetch the last (most recent) sensor reading from the database
    last_reading = SensorReading.query.order_by(SensorReading.timestamp.desc()).first()
    
    # Prepare data for the form fields based on the last reading
    last_data = {}
    if last_reading:
        last_data = {
            'nbcm_selected': last_reading.nbcm_selected.split(',') if last_reading.nbcm_selected else [],
            'humidity': last_reading.humidity,
            'temp': last_reading.temp,
            'time': last_reading.time,
            'date': last_reading.date
        }
    # Pass the last_data dictionary to the index.html template
    return render_template('index.html', last_data=last_data)

# Route to display the history of stored data
@app.route('/history')
def history():
    # Fetch all sensor readings from the database, ordered by timestamp (newest first)
    all_readings = SensorReading.query.order_by(SensorReading.timestamp.desc()).all()
    # Pass the list of readings to the new history.html template
    return render_template('history.html', readings=all_readings)

# NEW ROUTE: To clear all data from the history
@app.route('/clear_history', methods=['POST'])
def clear_history():
    try:
        # Delete all records from the SensorReading table
        num_deleted = db.session.query(SensorReading).delete()
        db.session.commit()
        flash(f'Successfully cleared {num_deleted} entries from history.', 'success')
        print(f"--- Cleared {num_deleted} entries from history ---")
    except Exception as e:
        db.session.rollback() # Rollback in case of error
        flash(f'Error clearing history: {e}', 'error')
        print(f"--- Error clearing history: {e} ---")
    return redirect(url_for('history'))

# API ROUTE: To provide sensor data for plotting
@app.route('/api/sensor_data')
def get_sensor_data():
    print("--- Fetching sensor data for API ---")
    # Fetch all sensor readings, ordered chronologically for plotting
    readings = SensorReading.query.order_by(SensorReading.timestamp.asc()).all()
    data = []
    for reading in readings:
        print(f"  Processing reading ID: {reading.id}")
        # Updated print statement to show (temp, id) format
        print(f"    Raw data: (temp: {reading.temp}, ID: {reading.id}), Raw time: {reading.time}, Raw formatted_log: {reading.formatted_log}")

        # Attempt to convert temperature to a float, handle 'N/A' or invalid values
        temp_val = None
        if reading.temp:
            try:
                temp_val = float(reading.temp)
            except ValueError:
                print(f"    Warning: Could not convert temperature '{reading.temp}' to float for reading ID {reading.id}")

        # Attempt to convert humidity to a float, handle 'N/A' or invalid values
        humidity_val = None
        if reading.humidity:
            try:
                humidity_val = float(reading.humidity)
            except ValueError:
                print(f"    Warning: Could not convert humidity '{reading.humidity}' to float for reading ID {reading.id}")

        # Parse NBCM statuses from formatted_log
        nbcm_statuses = {}
        if reading.formatted_log:
            parts = reading.formatted_log.split(',')
            for part in parts:
                if part.startswith('Nbcm '):
                    try:
                        nbcm_id_part, status = part.split(':')
                        # Extract just the number from "Nbcm X" (e.g., "Nbcm 1" -> "1")
                        nbcm_num = nbcm_id_part.split(' ')[1]
                        nbcm_statuses[f"NBCM{nbcm_num}"] = status
                    except (ValueError, IndexError): # Added IndexError for safer parsing
                        print(f"    Warning: Could not parse NBCM status from part '{part}' for reading ID {reading.id}")

        print(f"    Processed temp_val: {temp_val}, Processed humidity_val: {humidity_val}, Processed nbcm_statuses: {nbcm_statuses}")
        data.append({
            'id': reading.id, # ADDED: Include the reading ID
            'time': reading.time, # Keep as string for now, JavaScript will parse
            'temp': temp_val,
            'humidity': humidity_val, # Add humidity data
            'nbcm_statuses': nbcm_statuses # Add NBCM statuses
        })
    print(f"--- Sending {len(data)} sensor data entries ---")
    # Return the data as a JSON response
    return jsonify(data)


# Route to handle form submissions (from your HTML form or ESP32)
@app.route('/submit_form', methods=['POST'])
def submit_form():
    if request.method == 'POST':
        # Get the list of 'nbcm' values that were checked from the form
        nbcm_checked_list = request.form.getlist('nbcm')

        # --- Logic to build the specific formatted string as requested ---
        nbcm_statuses_parts = []
        # Define all possible NBCM IDs in order
        all_nbcms_ids = ['NBCM1', 'NBCM2', 'NBCM3', 'NBCM4', 'NBCM5']

        for i, nbcm_id in enumerate(all_nbcms_ids):
            # Check if the current nbcm_id was present in the list of checked boxes
            status = "active" if nbcm_id in nbcm_checked_list else "notactive"
            nbcm_statuses_parts.append(f"Nbcm {i+1}:{status}") # Use i+1 for Nbcm 1, Nbcm 2, etc.

        # Get other form data, providing 'N/A' as fallback if not present
        # This handles cases where a field might be empty or not sent by ESP32
        humidity_val = request.form.get('humidity') or 'N/A'
        temp_val = request.form.get('temp') or 'N/A'
        input_time = request.form.get('time') or 'N/A'
        input_date = request.form.get('date') or 'N/A'

        # Combine all parts into the final desired string format
        final_log_string = ",".join(nbcm_statuses_parts)
        final_log_string += f",humidity:{humidity_val}%,temp:{temp_val}"
        final_log_string += f",time:{input_time},date:{input_date}"
        # --- End of string formatting logic ---

        # Create a new SensorReading object with the received data
        new_reading = SensorReading(
            # Store the individual, comma-separated active NBCMs here (original behavior)
            nbcm_selected=",".join(nbcm_checked_list),
            humidity=humidity_val,
            temp=temp_val,
            time=input_time,
            date=input_date,
            # Store the newly formatted combined string in the dedicated column
            formatted_log=final_log_string
        )

        # Add the new reading to the database session
        db.session.add(new_reading)
        # Commit the session to save the data to the database
        db.session.commit()

        print("--- Data Saved to Database ---")
        print(f"Formatted Log String: {final_log_string}")
        print(f"DB Entry ID: {new_reading.id}")
        print("------------------------------")

        flash('Data successfully saved!', 'success') # Optional: Add a flash message
        # Redirect to the home page instead of history page
        return redirect(url_for('home'))

# NEW ROUTE: Export data to CSV (Excel compatible)
@app.route('/export_excel')
def export_excel():
    # Fetch all sensor readings
    all_readings = SensorReading.query.order_by(SensorReading.timestamp.asc()).all()

    # Create a string buffer to hold the CSV data
    si = io.StringIO()
    cw = csv.writer(si)

    # Write header row
    cw.writerow(['ID', 'NBCM Selected', 'Humidity', 'Temperature', 'Time', 'Date', 'Formatted Log'])

    # Write data rows
    for reading in all_readings:
        cw.writerow([
            reading.id,
            reading.nbcm_selected,
            reading.humidity,
            reading.temp,
            reading.time,
            reading.date,
            reading.formatted_log
        ])
    
    output = si.getvalue()

    # Create a Flask Response to send the CSV file
    response = Response(output, mimetype="text/csv")
    response.headers["Content-Disposition"] = "attachment; filename=sensor_data.csv"
    return response

# New route to serve the plot display HTML
@app.route('/plot_display')
def plot_display():
    return render_template('plot_display.html')


# This block ensures that the Flask development server runs only when
# the script is executed directly (not when imported as a module).
if __name__ == '__main__':
    # --- IMPORTANT: Create Database Tables ---
    # This line creates the 'site.db' file and all defined tables
    # if they don't already exist.
    # If you have changed your model (e.g., added new columns like 'formatted_log'),
    # you MUST delete the existing 'site.db' file before running this,
    # or use Flask-Migrate for proper database migrations in a production setup.
    with app.app_context():
        db.create_all()
    # ----------------------------------------

    app.run(debug=True) # Run the Flask application in debug mode
